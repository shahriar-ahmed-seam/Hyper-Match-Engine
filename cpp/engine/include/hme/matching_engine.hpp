// Matching_Engine NewOrder matching core.
//
// Implements limit-order matching for an incoming NewOrder against the resting
// opposite side of the Order_Book, in strict price-time priority. It operates
// purely on the pre-allocated OrderBook storage (hme/order_book.hpp): no I/O,
// no clock reads, no dynamic allocation, no exceptions, so it is hot-path safe.
//
// Matching of a NewOrder:
//
//   * Select the eligible opposite-side resting order with the best price, then
//     earliest arrival sequence, then lowest order identifier. The OrderBook
//     keeps the best price level at the head of each side and orders within a
//     level by (seq, order_id), so the order to match next is always
//     front_order()/level_head() of the best level.
//
//   * Execute each Trade at the *resting* order's limit price, with quantity
//     equal to the smaller of the incoming and resting remaining quantities.
//
//   * Continue matching the incoming order against successive eligible resting
//     orders until it is fully filled or no eligible opposite-side order
//     remains.
//
//   * Remove a resting order from the book as soon as its remaining quantity
//     reaches zero.
//
//   * Rest any unmatched remainder of the incoming order in the book at its
//     own limit price, preserving its arrival sequence for later time priority.
//
//   * Emit one Trade event per execution carrying a monotonic execution
//     sequence number, the trade price, the trade quantity, the incoming order
//     identifier, and the resting order identifier.
//
// process_cancel_order locates the resting order referenced by a CancelOrder;
// when present it is removed from the Order_Book and a single Ack
// (AckKind::Cancelled) carrying the order identifier is emitted. Because the
// book holds only resting orders, removal alone excludes the identifier from
// all subsequent matching. When the identifier is not resting, the book is left
// unchanged and a single cancellation Reject is emitted: a bounded retired-id
// tracker distinguishes an id that was once resting but has since left the book
// (fully filled or already cancelled -> RejectReason::NoLongerResting) from one
// the engine has never seen (-> RejectReason::OrderNotFound). Like matching,
// cancellation is allocation/exception/clock free.
//
// Incoming-order validation: before any matching, a NewOrder whose quantity
// falls outside the engine's permitted range [kMinEngineQuantity,
// kMaxEngineQuantity] or whose price falls outside [kMinPriceTicks,
// kMaxPriceTicks] is rejected. The engine emits a Reject event carrying the
// offending order's identifier and the appropriate RejectReason
// (InvalidQuantity / InvalidPrice) and leaves the Order_Book completely
// unchanged - no matching, no resting. The two-sink process_new_order overload
// performs this validation step; the single-sink overload assumes an
// already-valid NewOrder.

#ifndef HME_MATCHING_ENGINE_HPP
#define HME_MATCHING_ENGINE_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include "hme/binary_message.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

namespace hme::engine {

// ---------------------------------------------------------------------------
// RetiredIdSet: a bounded, allocation-free membership set of order identifiers
// that have left the Order_Book (fully filled during matching, or cancelled).
//
// It lets cancellation distinguish a cancel for an id that was once resting but
// is no longer (NoLongerResting) from a cancel for an id the engine has never
// seen (OrderNotFound).
//
// Implemented as a fixed-capacity open-addressing hash set (linear probing,
// back-shift deletion) paired with a FIFO ring recording insertion order. When
// the set is full a fresh insert evicts the OLDEST recorded id, so the whole
// structure is O(1) and strictly bounded - no std::unordered_set, no hot-path
// allocation.
//
// Because it is bounded, a very old retired id may eventually be evicted; a
// later cancel for such an id would then report OrderNotFound rather than
// NoLongerResting. That is acceptable: the Order_Book itself is bounded, so the
// engine retains only a bounded recent history of retired ids.
template <std::size_t Capacity>
class RetiredIdSet {
    static_assert(Capacity > 0, "RetiredIdSet capacity must be greater than zero");

    // Number of hash-table slots: a power of two at least twice the capacity,
    // so the load factor stays <= 0.5 (fast probing) and an empty slot always
    // exists for the open-addressing probe to terminate on.
    static constexpr std::size_t pow2_at_least(std::size_t n) noexcept {
        std::size_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }
    static constexpr std::size_t log2_floor(std::size_t n) noexcept {
        std::size_t l = 0;
        while ((std::size_t{1} << (l + 1)) <= n) {
            ++l;
        }
        return l;
    }

    static constexpr std::size_t kTableSize = pow2_at_least(2 * Capacity);
    static constexpr std::size_t kMask = kTableSize - 1;
    static constexpr std::size_t kBits = log2_floor(kTableSize);
    static constexpr std::size_t kNoSlot =
        std::numeric_limits<std::size_t>::max();

    struct Slot {
        std::uint64_t key = 0;
        bool occupied = false;
    };

public:
    // Largest number of retired ids the set retains (oldest evicted past this).
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // True when `id` is currently recorded as retired.
    bool contains(std::uint64_t id) const noexcept {
        return find_slot(id) != kNoSlot;
    }

    // Record `id` as retired. A no-op when already present; otherwise inserts,
    // first evicting the oldest recorded id when the set is at capacity (FIFO).
    void insert(std::uint64_t id) noexcept {
        if (find_slot(id) != kNoSlot) {
            return;  // already tracked; keep its existing FIFO position.
        }
        if (count_ == Capacity) {
            // Evict the oldest recorded id to stay bounded.
            const std::uint64_t oldest = ring_[ring_head_];
            erase(oldest);
            ring_head_ = (ring_head_ + 1) % Capacity;
            --count_;
        }
        table_insert(id);
        const std::size_t tail = (ring_head_ + count_) % Capacity;
        ring_[tail] = id;
        ++count_;
    }

    // Drop every recorded id, returning the set to its empty state.
    void clear() noexcept {
        for (Slot& s : slots_) {
            s = Slot{};
        }
        count_ = 0;
        ring_head_ = 0;
    }

private:
    static constexpr std::size_t home(std::uint64_t key) noexcept {
        // Fibonacci (multiplicative) hashing: take the high, well-mixed bits so
        // sequential order ids do not form one long linear-probe run.
        return static_cast<std::size_t>(
            (key * 0x9E3779B97F4A7C15ULL) >> (64 - kBits));
    }

    // Probe for `id`; returns its slot index or kNoSlot when absent.
    std::size_t find_slot(std::uint64_t id) const noexcept {
        std::size_t i = home(id);
        while (slots_[i].occupied) {
            if (slots_[i].key == id) {
                return i;
            }
            i = (i + 1) & kMask;
        }
        return kNoSlot;
    }

    // Insert `id` into the first free slot along its probe sequence. The caller
    // guarantees `id` is absent and that a free slot exists (load factor < 1).
    void table_insert(std::uint64_t id) noexcept {
        std::size_t i = home(id);
        while (slots_[i].occupied) {
            i = (i + 1) & kMask;
        }
        slots_[i].key = id;
        slots_[i].occupied = true;
    }

    // Remove `id` from the table with linear-probing back-shift deletion so the
    // probe sequences of other keys stay intact (no tombstones).
    void erase(std::uint64_t id) noexcept {
        std::size_t i = find_slot(id);
        if (i == kNoSlot) {
            return;
        }
        std::size_t j = i;
        while (true) {
            slots_[i].occupied = false;
            std::size_t k;
            do {
                j = (j + 1) & kMask;
                if (!slots_[j].occupied) {
                    return;  // reached the end of this cluster.
                }
                k = home(slots_[j].key);
                // Keep advancing while slot j's key cannot move into the hole at
                // i (its home lies cyclically within (i, j]).
            } while ((i <= j) ? (i < k && k <= j) : (i < k || k <= j));
            slots_[i] = slots_[j];
            i = j;
        }
    }

    std::array<Slot, kTableSize> slots_{};   // open-addressing table.
    std::array<std::uint64_t, Capacity> ring_{};  // insertion-order FIFO.
    std::size_t ring_head_ = 0;              // index of the oldest recorded id.
    std::size_t count_ = 0;                  // number of recorded ids.
};

// Matching core over a fixed-capacity Order_Book.
//
//   MaxOrders      - capacity of the resting-order pool (both sides combined).
//   MaxPriceLevels - capacity of the price-level pool (both sides combined).
//
// Both capacities are fixed at construction; matching never grows them. When
// the incoming remainder cannot be rested because a pool is exhausted,
// insertion fails gracefully and the remainder is dropped rather than
// allocating; the book is otherwise left consistent.
template <std::size_t MaxOrders, std::size_t MaxPriceLevels>
class MatchingEngine {
public:
    using Book = OrderBook<MaxOrders, MaxPriceLevels>;

    MatchingEngine() noexcept = default;

    // Read-only / mutable access to the underlying book (for the process loop,
    // cancellation, the integrity guard, and tests).
    const Book& book() const noexcept { return book_; }
    Book& book() noexcept { return book_; }

    // The execution sequence number that will be assigned to the next Trade.
    // Monotonically non-decreasing across the engine's lifetime.
    std::uint64_t next_exec_seq() const noexcept { return next_exec_seq_; }

    // Match an incoming NewOrder against the opposite side of the book in
    // price-time priority, delivering each resulting Trade to `emit_trade` in
    // execution order, and resting any unmatched remainder.
    //
    // `emit_trade` is any callable invocable as `emit_trade(const Trade&)`.
    // Trades are delivered as they occur, so the callback observes them in
    // execution order. The caller owns where the events go (egress ring,
    // vector, etc.); this method never allocates.
    //
    // The incoming order is assumed already validated. `incoming.seq` is the
    // arrival sequence used for time priority when the remainder rests.
    template <typename TradeSink>
    void process_new_order(const NewOrder& incoming, TradeSink&& emit_trade) {
        const Side opposite =
            (incoming.side == Side::Buy) ? Side::Sell : Side::Buy;
        std::uint32_t remaining = incoming.quantity;

        // Match against successive best opposite-side resting orders while the
        // incoming order still has quantity and an eligible counterpart exists.
        while (remaining > 0) {
            const PriceLevel* best = best_level(opposite);
            if (best == nullptr) {
                break;  // opposite side is empty -> no eligible counterpart.
            }
            // Price eligibility: a buy crosses asks at or below its limit; a
            // sell crosses bids at or above its limit. Because the best level is
            // the most aggressive price on that side, once it is out of range no
            // further level can match.
            if (!price_crosses(incoming.side, incoming.price_ticks,
                               best->price_ticks)) {
                break;
            }

            // The head of the best level is the highest-priority resting order:
            // earliest (seq, order_id).
            const NodeIndex node = book_.level_head(*best);
            RestingOrder& resting = book_.order_at(node);

            // Trade quantity is the smaller of the two remaining quantities;
            // always strictly positive here.
            const std::uint32_t trade_qty =
                std::min(remaining, resting.remaining_qty);

            // Execute at the resting order's limit price and emit a fully
            // populated Trade event.
            Trade trade;
            trade.exec_seq = next_exec_seq_++;
            trade.price_ticks = resting.price_ticks;
            trade.quantity = trade_qty;
            trade.incoming_id = incoming.order_id;
            trade.resting_id = resting.order_id;
            emit_trade(static_cast<const Trade&>(trade));

            remaining -= trade_qty;
            resting.remaining_qty -= trade_qty;

            // A fully filled resting order leaves the book immediately. Record
            // its id as retired so a later cancel for it is distinguished as
            // NoLongerResting rather than OrderNotFound.
            if (resting.remaining_qty == 0) {
                retired_.insert(resting.order_id);
                book_.remove_order(node);
            }
        }

        // Any unmatched remainder rests at the incoming order's own limit
        // price, keeping its arrival sequence for time priority. If the book's
        // pools are exhausted the insert fails without allocating; the remainder
        // is then simply not rested.
        if (remaining > 0) {
            book_.insert_order(incoming.order_id, incoming.side,
                               incoming.price_ticks, incoming.seq, remaining);
        }
    }

    // Determine whether an incoming NewOrder satisfies the engine's matching
    // domain. Returns the RejectReason for the first violation found, or
    // std::nullopt when the order is valid. Pure, constexpr,
    // allocation/exception/clock free.
    //
    // The engine-side quantity domain is [kMinEngineQuantity,
    // kMaxEngineQuantity] = [1, 1,000,000]; the price domain is
    // [kMinPriceTicks, kMaxPriceTicks] (JSON 0.01 .. 999,999,999.99 as ticks).
    // Quantity is checked first, then price, so an order violating both reports
    // InvalidQuantity.
    static constexpr std::optional<RejectReason> validate_new_order(
        const NewOrder& incoming) noexcept {
        if (incoming.quantity < limits::kMinEngineQuantity ||
            incoming.quantity > limits::kMaxEngineQuantity) {
            return RejectReason::InvalidQuantity;
        }
        if (incoming.price_ticks < limits::kMinPriceTicks ||
            incoming.price_ticks > limits::kMaxPriceTicks) {
            return RejectReason::InvalidPrice;
        }
        return std::nullopt;
    }

    // Validate then process an incoming NewOrder. If the order is outside the
    // engine's permitted quantity or price range, emit a single Reject event
    // (carrying the order identifier and the appropriate RejectReason) via
    // `emit_reject`, leave the Order_Book completely unchanged, and return
    // false. Otherwise match it exactly as the single-sink overload does,
    // delivering trades to `emit_trade`, and return true.
    //
    // `emit_trade` is invocable as `emit_trade(const Trade&)`; `emit_reject` is
    // invocable as `emit_reject(const Reject&)`. Neither this method nor the
    // matching it delegates to allocates, throws, or reads a clock, so it is
    // hot-path safe.
    template <typename TradeSink, typename RejectSink>
    bool process_new_order(const NewOrder& incoming, TradeSink&& emit_trade,
                           RejectSink&& emit_reject) {
        const std::optional<RejectReason> reason = validate_new_order(incoming);
        if (reason.has_value()) {
            // Invalid order: emit a rejection and preserve the book unchanged
            // (no matching, no resting).
            Reject reject;
            reject.order_id = incoming.order_id;
            reject.reason = *reason;
            emit_reject(static_cast<const Reject&>(reject));
            return false;
        }
        process_new_order(incoming, std::forward<TradeSink>(emit_trade));
        return true;
    }

    // Process a CancelOrder against the book.
    //
    // If the referenced order is currently resting, remove it from the
    // Order_Book and emit exactly one cancellation acknowledgement carrying its
    // identifier with AckKind::Cancelled; return true. Once removed, the
    // identifier is no longer present in the book, so it is excluded from all
    // subsequent matching without any further bookkeeping.
    //
    // If the referenced order is not present in the book, leave the Order_Book
    // unchanged and emit exactly one cancellation rejection carrying the
    // identifier; return false. The reason distinguishes an id that was once
    // resting but has since left the book - fully filled or already cancelled ->
    // RejectReason::NoLongerResting - from one never seen ->
    // RejectReason::OrderNotFound, using the bounded retired-id tracker.
    //
    // `emit_ack` is invocable as `emit_ack(const Ack&)`; `emit_reject` is
    // invocable as `emit_reject(const Reject&)`. This method never allocates,
    // throws, or reads a clock, so it is hot-path safe.
    template <typename AckSink, typename RejectSink>
    bool process_cancel_order(const CancelOrder& cancel, AckSink&& emit_ack,
                              RejectSink&& emit_reject) {
        const NodeIndex node = book_.find_order(cancel.order_id);
        if (node == kNullNode) {
            // No resting order with this identifier: reject without effect.
            // Distinguish an id that was once resting but has since left the
            // book (fully filled or already cancelled -> NoLongerResting) from
            // one the engine has never seen (OrderNotFound).
            Reject reject;
            reject.order_id = cancel.order_id;
            reject.reason = retired_.contains(cancel.order_id)
                                ? RejectReason::NoLongerResting
                                : RejectReason::OrderNotFound;
            emit_reject(static_cast<const Reject&>(reject));
            return false;
        }

        // Resting order found: remove it, record its id as retired (so a later
        // cancel reports NoLongerResting), and acknowledge the cancellation.
        book_.remove_order(node);
        retired_.insert(cancel.order_id);
        Ack ack;
        ack.order_id = cancel.order_id;
        ack.kind = AckKind::Cancelled;
        emit_ack(static_cast<const Ack&>(ack));
        return true;
    }

    // Drop the bounded retired-id history, returning the engine to a state with
    // no record of previously-resting orders. Used at startup so a re-initialized
    // engine does not classify a stale id as NoLongerResting.
    void clear_retired() noexcept { retired_.clear(); }

private:
    // Best (most aggressive) price level on a side, or nullptr if empty.
    const PriceLevel* best_level(Side side) const noexcept {
        return (side == Side::Buy) ? book_.best_bid() : book_.best_ask();
    }

    // True when an incoming order with `incoming_price` on `incoming_side` is
    // eligible to trade against a resting price `resting_price` on the opposite
    // side: a buy matches asks priced at or below it; a sell matches bids
    // priced at or above it.
    static constexpr bool price_crosses(Side incoming_side,
                                        std::uint64_t incoming_price,
                                        std::uint64_t resting_price) noexcept {
        return (incoming_side == Side::Buy) ? (resting_price <= incoming_price)
                                            : (resting_price >= incoming_price);
    }

    Book book_{};
    std::uint64_t next_exec_seq_ = 1;  // monotonic execution sequence.

    // Bounded history of order ids that have left the book (fully filled or
    // cancelled), sized to the resting-order pool capacity. Lets a cancel miss
    // be classified as NoLongerResting (once resting) vs OrderNotFound (never
    // seen). Bounded: a very old retired id may be evicted and later report
    // OrderNotFound (acceptable; the book is bounded).
    RetiredIdSet<MaxOrders> retired_{};
};

}  // namespace hme::engine

#endif  // HME_MATCHING_ENGINE_HPP
