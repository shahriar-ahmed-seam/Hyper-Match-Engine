// Order_Book integrity invariant checks and the atomic integrity guard.
//
// After every state transition the Matching_Engine must guarantee that the
// Order_Book is internally consistent. This header provides:
//
//   * Pure invariant-check functions over a const OrderBook&:
//       - check_price_ordering   : bids strictly descending, asks strictly
//                                  ascending, and (when both sides are present)
//                                  Best_Bid < Best_Ask, i.e. an un-crossed book.
//       - check_positive_quantity: every Resting_Order has remaining quantity
//                                  strictly greater than zero - equivalently,
//                                  any order whose quantity reached zero has
//                                  been removed.
//       - check_quantity_conservation: for a single incoming Order, the
//                                  incoming quantity equals the sum of matched
//                                  Trade quantities plus the quantity added to
//                                  the book.
//
//   * IntegrityGuard: an atomic guard that snapshots the book into pre-reserved
//     storage, runs an operation, validates the invariants on the resulting
//     state, and - if any invariant is violated - restores the pre-operation
//     book state and reports the violated invariant by name. A committed
//     operation is therefore all-or-nothing: either it leaves the book
//     consistent or it leaves the book exactly as it was.
//
// Like the rest of the matching core, everything here is allocation/exception/
// clock free and so is hot-path safe. The guard's snapshot lives in a
// pre-reserved OrderBook member (sized at construction, never grown), so taking
// a snapshot and restoring from it performs no dynamic allocation.

#ifndef HME_INTEGRITY_GUARD_HPP
#define HME_INTEGRITY_GUARD_HPP

#include <cstddef>
#include <cstdint>
#include <utility>

#include "hme/binary_message.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

namespace hme::engine {

// ---------------------------------------------------------------------------
// Invariant: the Order_Book integrity invariants enforced after each operation.
// `None` denotes "no violation".
// ---------------------------------------------------------------------------
enum class Invariant : std::uint8_t {
    None = 0,
    PriceOrdering = 1,         // un-crossed, sorted book.
    PositiveQuantity = 2,      // no zero-qty resting order.
    QuantityConservation = 3,  // incoming == traded + rested.
};

// A stable, human-readable name for an invariant, used when emitting an error
// that identifies the violated invariant. The returned pointer is a string
// literal with static lifetime.
constexpr const char* invariant_name(Invariant inv) noexcept {
    switch (inv) {
        case Invariant::None: return "none";
        case Invariant::PriceOrdering: return "price-ordering";
        case Invariant::PositiveQuantity: return "positive-quantity";
        case Invariant::QuantityConservation: return "quantity-conservation";
    }
    return "unknown";
}

// The error reported when a committed operation violates an integrity
// invariant. Carries both the machine-readable invariant and its name so the
// caller can log, surface, or convert it into a Reject Binary_Message.
struct IntegrityError {
    Invariant invariant = Invariant::None;
    const char* name = "none";

    friend bool operator==(const IntegrityError&, const IntegrityError&) = default;
};

// Build the Reject Binary_Message emitted for an integrity violation against a
// given incoming order. All integrity violations map to
// RejectReason::IntegrityViolation; the specific invariant is conveyed
// separately via IntegrityError.
constexpr Reject make_integrity_reject(std::uint64_t order_id) noexcept {
    Reject reject;
    reject.order_id = order_id;
    reject.reason = RejectReason::IntegrityViolation;
    return reject;
}

// ---------------------------------------------------------------------------
// Pure invariant-check functions (over book state).
//
// These walk the book through its public best_bid()/best_ask()/level_after()/
// level_head()/order_at() accessors only, so they make no assumptions about the
// pool internals and never mutate, allocate, throw, or read a clock.
// ---------------------------------------------------------------------------

// Bids are strictly descending in price, asks strictly ascending, and whenever
// both a Best_Bid and a Best_Ask exist the Best_Bid price is strictly less than
// the Best_Ask price (the book is not crossed).
template <std::size_t MaxOrders, std::size_t MaxPriceLevels>
bool check_price_ordering(
    const OrderBook<MaxOrders, MaxPriceLevels>& book) noexcept {
    // Bids: each level's price must be strictly below the previous (better) one.
    const PriceLevel* prev = nullptr;
    for (const PriceLevel* lvl = book.best_bid(); lvl != nullptr;
         lvl = book.level_after(*lvl)) {
        if (prev != nullptr && lvl->price_ticks >= prev->price_ticks) {
            return false;
        }
        prev = lvl;
    }
    // Asks: each level's price must be strictly above the previous (better) one.
    prev = nullptr;
    for (const PriceLevel* lvl = book.best_ask(); lvl != nullptr;
         lvl = book.level_after(*lvl)) {
        if (prev != nullptr && lvl->price_ticks <= prev->price_ticks) {
            return false;
        }
        prev = lvl;
    }
    // Un-crossed book: Best_Bid strictly below Best_Ask when both exist.
    const PriceLevel* best_bid = book.best_bid();
    const PriceLevel* best_ask = book.best_ask();
    if (best_bid != nullptr && best_ask != nullptr &&
        best_bid->price_ticks >= best_ask->price_ticks) {
        return false;
    }
    return true;
}

// Every Resting_Order in the book has remaining quantity strictly greater than
// zero (a fully filled order must have been removed).
template <std::size_t MaxOrders, std::size_t MaxPriceLevels>
bool check_positive_quantity(
    const OrderBook<MaxOrders, MaxPriceLevels>& book) noexcept {
    for (const Side side : {Side::Buy, Side::Sell}) {
        const PriceLevel* start =
            (side == Side::Buy) ? book.best_bid() : book.best_ask();
        for (const PriceLevel* lvl = start; lvl != nullptr;
             lvl = book.level_after(*lvl)) {
            for (NodeIndex node = book.level_head(*lvl); node != kNullNode;
                 node = book.order_at(node).next) {
                // remaining_qty is unsigned, so "<= 0" reduces to "== 0".
                if (book.order_at(node).remaining_qty == 0) {
                    return false;
                }
            }
        }
    }
    return true;
}

// For a single incoming Order, the incoming quantity must equal the sum of all
// matched Trade quantities generated for that Order plus the quantity added to
// the Order_Book as a Resting_Order. Pure arithmetic over 64-bit accumulators
// to avoid overflow when summing many trades.
constexpr bool check_quantity_conservation(std::uint64_t incoming_qty,
                                           std::uint64_t traded_qty,
                                           std::uint64_t rested_qty) noexcept {
    return incoming_qty == traded_qty + rested_qty;
}

// Evaluate the two book-state invariants (price ordering, positive quantity)
// and return the first one violated, or Invariant::None when both hold.
// Conservation is per-operation and is checked separately by the guard.
template <std::size_t MaxOrders, std::size_t MaxPriceLevels>
Invariant find_state_violation(
    const OrderBook<MaxOrders, MaxPriceLevels>& book) noexcept {
    if (!check_price_ordering(book)) {
        return Invariant::PriceOrdering;
    }
    if (!check_positive_quantity(book)) {
        return Invariant::PositiveQuantity;
    }
    return Invariant::None;
}

// ---------------------------------------------------------------------------
// OpOutcome: what a guarded operation reports back to the guard so it can also
// check quantity conservation.
//
// Book-state invariants (price ordering, positive quantity) are checked by the
// guard directly against the post-operation book and need no input here. For
// operations that consume an incoming Order's quantity (NewOrder matching), set
// `check_conservation` to true and populate the three quantities; for
// operations that do not (e.g. CancelOrder), leave `check_conservation` false.
// ---------------------------------------------------------------------------
struct OpOutcome {
    bool check_conservation = false;  // true for incoming-order operations.
    std::uint64_t incoming_qty = 0;   // the incoming Order's quantity.
    std::uint64_t traded_qty = 0;     // sum of matched Trade quantities.
    std::uint64_t rested_qty = 0;     // quantity added to the book.

    // Convenience for a CancelOrder or any operation that does not consume an
    // incoming order's quantity: no conservation check applies.
    static constexpr OpOutcome no_conservation() noexcept { return OpOutcome{}; }

    // Convenience for an incoming-order operation that should be checked for
    // quantity conservation.
    static constexpr OpOutcome conserving(std::uint64_t incoming,
                                          std::uint64_t traded,
                                          std::uint64_t rested) noexcept {
        return OpOutcome{/*check_conservation=*/true, incoming, traded, rested};
    }
};

// ---------------------------------------------------------------------------
// IntegrityGuard: atomic snapshot / validate / restore wrapper.
//
// `MaxOrders` / `MaxPriceLevels` must match the OrderBook being guarded. The
// guard owns a pre-reserved OrderBook used purely as snapshot storage, so
// snapshot() and restore() are plain member-wise array copies with no dynamic
// allocation. The guard never mutates the live book except via restore().
// ---------------------------------------------------------------------------
template <std::size_t MaxOrders, std::size_t MaxPriceLevels>
class IntegrityGuard {
public:
    using Book = OrderBook<MaxOrders, MaxPriceLevels>;

    IntegrityGuard() noexcept = default;

    // Capture the current book state into pre-reserved snapshot storage.
    // Allocation-free (copies into the owned snapshot member).
    void snapshot(const Book& book) noexcept { snapshot_ = book; }

    // Overwrite the live book with the captured snapshot, undoing any mutation
    // performed since the last snapshot(). Allocation-free.
    void restore(Book& book) const noexcept { book = snapshot_; }

    // Run an operation against `book` atomically with respect to the integrity
    // invariants.
    //
    //   1. Snapshot the pre-operation book state.
    //   2. Invoke `op()`, which mutates `book` and returns an OpOutcome
    //      describing whether/which quantity-conservation check applies.
    //   3. Validate the post-operation book against the price-ordering and
    //      positive-quantity invariants and, when requested, the
    //      quantity-conservation invariant.
    //   4. If any invariant is violated, restore the pre-operation state and
    //      report the violated invariant to `emit_error` (an IntegrityError),
    //      returning false. Otherwise commit (leave the mutation in place) and
    //      return true.
    //
    // `op` is invocable as `OpOutcome op()`; `emit_error` is invocable as
    // `emit_error(const IntegrityError&)`. Neither this method nor a
    // well-behaved `op` allocates, throws, or reads a clock.
    template <typename Op, typename ErrorSink>
    bool commit(Book& book, Op&& op, ErrorSink&& emit_error) {
        snapshot(book);
        const OpOutcome outcome = op();
        const Invariant violated = evaluate(book, outcome);
        if (violated != Invariant::None) {
            restore(book);
            emit_error(IntegrityError{violated, invariant_name(violated)});
            return false;
        }
        return true;
    }

    // Evaluate every applicable invariant against the (already mutated) book and
    // the operation outcome, returning the first violation or Invariant::None.
    // Exposed for callers/tests that perform their own snapshot management.
    Invariant evaluate(const Book& book, const OpOutcome& outcome) const noexcept {
        const Invariant state = find_state_violation(book);
        if (state != Invariant::None) {
            return state;
        }
        if (outcome.check_conservation &&
            !check_quantity_conservation(outcome.incoming_qty,
                                         outcome.traded_qty,
                                         outcome.rested_qty)) {
            return Invariant::QuantityConservation;
        }
        return Invariant::None;
    }

private:
    Book snapshot_{};  // pre-reserved snapshot storage (no hot-path growth).
};

}  // namespace hme::engine

#endif  // HME_INTEGRITY_GUARD_HPP
