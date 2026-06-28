// Order_Book storage with pre-allocated pools.
//
// Provides the storage data structures for the Limit Order Book. Matching
// logic, the integrity guard, and the allocation counter live separately; this
// file is concerned only with how resting orders and price levels are stored,
// ordered, and looked up.
//
// Design drivers:
//
//   * Zero hot-path allocation. Every resting order and every price level lives
//     in a fixed-capacity pool whose storage is held inline (std::array
//     members). The pools are sized at construction (startup) via compile-time
//     capacities, mirroring the RingBuffer pattern, and never grow during
//     operation. Free slots are tracked with an intrusive free list, so
//     acquiring/releasing a node is O(1) and allocation-free. When a pool is
//     exhausted, insertion fails gracefully (returns false) rather than
//     allocating.
//
//   * Price ordering by side. Bids are kept descending (best bid = highest
//     price) and asks ascending (best ask = lowest price), so the best level on
//     each side is always at the head of that side's level list.
//
//   * FIFO within a price level by (seq, order_id). Resting orders at one price
//     are ordered by earliest arrival sequence number first, ties broken by the
//     lower order identifier. The order at the head of a level is therefore
//     always the one that must match first.
//
// All operations here are pure in-memory mutations: no I/O, no clock reads, no
// exceptions, no dynamic allocation.

#ifndef HME_ORDER_BOOK_HPP
#define HME_ORDER_BOOK_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "hme/wire_protocol.hpp"

namespace hme::engine {

// Handle into a pool. A node is referenced by its slot index; kNullNode marks
// "no node" (end of a list, empty book, exhausted pool).
using NodeIndex = std::uint32_t;
inline constexpr NodeIndex kNullNode = std::numeric_limits<NodeIndex>::max();

// ---------------------------------------------------------------------------
// FreeListPool: a fixed-capacity, pre-allocated object pool.
//
// All `Capacity` slots are reserved inline when the pool is constructed. A
// singly-linked free list (threaded through a parallel index array) tracks
// which slots are available, so acquire()/release() are O(1) and never
// allocate. acquire() returns kNullNode when the pool is exhausted instead of
// growing (no hot-path growth).
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity>
class FreeListPool {
    static_assert(Capacity > 0, "FreeListPool capacity must be greater than zero");
    static_assert(Capacity <= static_cast<std::size_t>(kNullNode),
                  "FreeListPool capacity must be addressable by NodeIndex");

public:
    using size_type = std::size_t;

    FreeListPool() noexcept { reset(); }

    // The fixed number of nodes this pool can hold.
    static constexpr size_type capacity() noexcept { return Capacity; }

    // Number of nodes currently in use.
    size_type size() const noexcept { return in_use_; }
    bool empty() const noexcept { return in_use_ == 0; }
    bool full() const noexcept { return in_use_ == Capacity; }

    // Reserve a free slot. Returns its index, or kNullNode if the pool is full.
    // O(1), allocation-free.
    NodeIndex acquire() noexcept {
        if (free_head_ == kNullNode) {
            return kNullNode;  // exhausted -> caller applies back-pressure.
        }
        const NodeIndex index = free_head_;
        free_head_ = next_free_[index];
        ++in_use_;
        return index;
    }

    // Return a previously acquired slot to the free list. O(1).
    void release(NodeIndex index) noexcept {
        next_free_[index] = free_head_;
        free_head_ = index;
        --in_use_;
    }

    // Restore the pool to the all-free state (drops every node). Used at
    // startup and by OrderBook::clear().
    void reset() noexcept {
        for (size_type i = 0; i < Capacity; ++i) {
            next_free_[i] = (i + 1 == Capacity)
                                ? kNullNode
                                : static_cast<NodeIndex>(i + 1);
        }
        free_head_ = 0;
        in_use_ = 0;
    }

    T& operator[](NodeIndex index) noexcept { return slots_[index]; }
    const T& operator[](NodeIndex index) const noexcept { return slots_[index]; }

private:
    std::array<T, Capacity> slots_{};        // pre-allocated, inline storage.
    std::array<NodeIndex, Capacity> next_free_{};  // free-list threading.
    NodeIndex free_head_ = kNullNode;        // head of the free list.
    size_type in_use_ = 0;                   // number of nodes in use.
};

// ---------------------------------------------------------------------------
// RestingOrder: a Limit_Order (or its unmatched remainder) held in the book.
//
// `next`/`prev` thread the FIFO list within a single price level, ordered by
// (seq, order_id). `seq` is the monotonic arrival sequence assigned on ingress;
// it drives time priority.
// ---------------------------------------------------------------------------
struct RestingOrder {
    std::uint64_t order_id = 0;
    std::uint64_t seq = 0;            // arrival sequence (time priority).
    std::uint64_t price_ticks = 0;    // mirrors the owning level's price.
    std::uint32_t remaining_qty = 0;  // strictly > 0 while resting.
    Side side = Side::Buy;

    // Intrusive FIFO links within the owning PriceLevel (kNullNode at ends).
    NodeIndex next = kNullNode;  // toward the tail (later (seq, order_id)).
    NodeIndex prev = kNullNode;  // toward the head (earlier (seq, order_id)).

    friend bool operator==(const RestingOrder&, const RestingOrder&) = default;
};

// True when resting order `a` has strictly higher priority than `b` within a
// price level: earlier arrival sequence first, ties broken by lower order_id.
constexpr bool has_priority_over(std::uint64_t a_seq, std::uint64_t a_id,
                                 std::uint64_t b_seq, std::uint64_t b_id) noexcept {
    return (a_seq < b_seq) || (a_seq == b_seq && a_id < b_id);
}

// ---------------------------------------------------------------------------
// PriceLevel: all resting orders sharing one limit price, in FIFO order.
//
// `head` is the highest-priority order (earliest (seq, order_id)) and the next
// to match; `tail` is the lowest-priority order. `next_level`/`prev_level`
// thread the per-side sorted level list (descending for bids, ascending for
// asks).
// ---------------------------------------------------------------------------
struct PriceLevel {
    std::uint64_t price_ticks = 0;
    std::uint32_t order_count = 0;

    // FIFO list of RestingOrder nodes at this price (by (seq, order_id)).
    NodeIndex head = kNullNode;  // first to match.
    NodeIndex tail = kNullNode;  // last to match.

    // Intrusive links within the owning side's sorted level list.
    NodeIndex next_level = kNullNode;  // toward worse prices.
    NodeIndex prev_level = kNullNode;  // toward better prices.
};

// ---------------------------------------------------------------------------
// OrderBook: the full book, backed entirely by pre-allocated pools.
//
//   MaxOrders      - capacity of the resting-order pool (both sides combined).
//   MaxPriceLevels - capacity of the price-level pool (both sides combined).
//
// Both capacities are fixed at construction; no operation grows them.
// Insertions that would exceed a pool's capacity fail and leave the book
// unchanged.
// ---------------------------------------------------------------------------
template <std::size_t MaxOrders, std::size_t MaxPriceLevels>
class OrderBook {
public:
    using size_type = std::size_t;

    static constexpr size_type order_capacity() noexcept { return MaxOrders; }
    static constexpr size_type level_capacity() noexcept { return MaxPriceLevels; }

    // Number of resting orders currently in the book (both sides).
    size_type order_count() const noexcept { return orders_.size(); }
    // Number of distinct price levels currently in the book (both sides).
    size_type level_count() const noexcept { return levels_.size(); }
    bool empty() const noexcept { return orders_.empty(); }

    // ---- Best-of-book access -----------------------------------------------

    // Best bid level (highest-priced resting buy) or nullptr if no bids.
    const PriceLevel* best_bid() const noexcept {
        return bid_head_ == kNullNode ? nullptr : &levels_[bid_head_];
    }
    // Best ask level (lowest-priced resting sell) or nullptr if no asks.
    const PriceLevel* best_ask() const noexcept {
        return ask_head_ == kNullNode ? nullptr : &levels_[ask_head_];
    }

    // The highest-priority resting order on a side (head of the best level), or
    // nullptr when that side is empty. This is the order matching consumes
    // first.
    const RestingOrder* front_order(Side side) const noexcept {
        const NodeIndex level = (side == Side::Buy) ? bid_head_ : ask_head_;
        if (level == kNullNode) {
            return nullptr;
        }
        const NodeIndex head = levels_[level].head;
        return head == kNullNode ? nullptr : &orders_[head];
    }

    // ---- Insertion ----------------------------------------------------------

    // Insert a resting order, preserving per-side price ordering and within-
    // level (seq, order_id) FIFO ordering. Returns true on success.
    //
    // Returns false (and leaves the book unchanged) if either pool is exhausted
    // or a new price level is required but the level pool is full. No dynamic
    // allocation occurs in any case.
    bool insert_order(std::uint64_t order_id, Side side,
                      std::uint64_t price_ticks, std::uint64_t seq,
                      std::uint32_t remaining_qty) noexcept {
        if (orders_.full()) {
            return false;  // no order slot -> reject without allocation.
        }

        const NodeIndex level = find_or_create_level(side, price_ticks);
        if (level == kNullNode) {
            return false;  // no level slot -> reject without allocation.
        }

        const NodeIndex node = orders_.acquire();
        // Safe: orders_.full() was false above, so acquire() cannot fail here.
        RestingOrder& ro = orders_[node];
        ro.order_id = order_id;
        ro.seq = seq;
        ro.price_ticks = price_ticks;
        ro.remaining_qty = remaining_qty;
        ro.side = side;
        ro.next = kNullNode;
        ro.prev = kNullNode;

        insert_into_level(level, node);
        return true;
    }

    // ---- Removal ------------------------------------------------------------

    // Remove a specific resting order by its pool handle. The handle must refer
    // to an order currently in the book. Empties and removes the price level
    // when its last order leaves.
    void remove_order(NodeIndex node) noexcept {
        RestingOrder& ro = orders_[node];
        const NodeIndex level = locate_level(ro.side, ro.price_ticks);
        // level is guaranteed to exist for an in-book order.
        unlink_from_level(level, node);
        orders_.release(node);
        if (levels_[level].order_count == 0) {
            remove_level(ro.side, level);
        }
    }

    // Locate a resting order by identifier. Returns its pool handle, or
    // kNullNode if no such order is resting. Lets cancellation and tests resolve
    // an id to a node; the scan is over live orders only and performs no
    // allocation.
    NodeIndex find_order(std::uint64_t order_id) const noexcept {
        for (NodeIndex level = bid_head_; level != kNullNode;
             level = levels_[level].next_level) {
            const NodeIndex found = find_in_level(level, order_id);
            if (found != kNullNode) {
                return found;
            }
        }
        for (NodeIndex level = ask_head_; level != kNullNode;
             level = levels_[level].next_level) {
            const NodeIndex found = find_in_level(level, order_id);
            if (found != kNullNode) {
                return found;
            }
        }
        return kNullNode;
    }

    // Mutable / const access to a resting order by handle.
    RestingOrder& order_at(NodeIndex node) noexcept { return orders_[node]; }
    const RestingOrder& order_at(NodeIndex node) const noexcept {
        return orders_[node];
    }

    // ---- Iteration ----------------------------------------------------------

    // Handle of the first (highest-priority) order at a level, for traversal
    // via RestingOrder::next. kNullNode when the level is empty.
    NodeIndex level_head(const PriceLevel& level) const noexcept {
        return level.head;
    }

    // The next price level on the same side (toward worse prices), or nullptr
    // at the end of that side's list. Lets matching and tests walk a side from
    // best_bid()/best_ask() downward without exposing pool handles.
    const PriceLevel* level_after(const PriceLevel& level) const noexcept {
        return level.next_level == kNullNode ? nullptr
                                             : &levels_[level.next_level];
    }

    // Drop every order and level, returning the book to its empty state.
    void clear() noexcept {
        orders_.reset();
        levels_.reset();
        bid_head_ = kNullNode;
        ask_head_ = kNullNode;
    }

private:
    // True when, on `side`, price `a` ranks ahead of price `b` in the level
    // list: higher prices first for bids, lower prices first for asks.
    static constexpr bool price_ranks_first(Side side, std::uint64_t a,
                                            std::uint64_t b) noexcept {
        return (side == Side::Buy) ? (a > b) : (a < b);
    }

    NodeIndex& side_head(Side side) noexcept {
        return (side == Side::Buy) ? bid_head_ : ask_head_;
    }

    // Find the level node for an existing price on a side, or kNullNode.
    NodeIndex locate_level(Side side, std::uint64_t price_ticks) const noexcept {
        const NodeIndex head = (side == Side::Buy) ? bid_head_ : ask_head_;
        for (NodeIndex level = head; level != kNullNode;
             level = levels_[level].next_level) {
            const std::uint64_t lp = levels_[level].price_ticks;
            if (lp == price_ticks) {
                return level;
            }
            // Levels are sorted best-first; once the target would rank ahead of
            // the current level, it cannot appear later in the list.
            if (price_ranks_first(side, price_ticks, lp)) {
                return kNullNode;
            }
        }
        return kNullNode;
    }

    // Find an existing level for (side, price) or create and link a new one in
    // sorted position. Returns kNullNode only when a new level is needed but
    // the level pool is exhausted.
    NodeIndex find_or_create_level(Side side, std::uint64_t price_ticks) noexcept {
        NodeIndex& head = side_head(side);

        // Walk the sorted list to find the price or the insertion point.
        NodeIndex prev = kNullNode;
        NodeIndex cur = head;
        while (cur != kNullNode) {
            PriceLevel& level = levels_[cur];
            if (level.price_ticks == price_ticks) {
                return cur;  // existing level.
            }
            if (price_ranks_first(side, price_ticks, level.price_ticks)) {
                break;  // insertion point: new price ranks before `cur`.
            }
            prev = cur;
            cur = level.next_level;
        }

        if (levels_.full()) {
            return kNullNode;  // cannot create a new level without allocating.
        }
        const NodeIndex node = levels_.acquire();
        PriceLevel& level = levels_[node];
        level.price_ticks = price_ticks;
        level.order_count = 0;
        level.head = kNullNode;
        level.tail = kNullNode;

        // Link `node` between `prev` and `cur`.
        level.prev_level = prev;
        level.next_level = cur;
        if (prev == kNullNode) {
            head = node;
        } else {
            levels_[prev].next_level = node;
        }
        if (cur != kNullNode) {
            levels_[cur].prev_level = node;
        }
        return node;
    }

    // Unlink and release an (already empty) level from its side's list.
    void remove_level(Side side, NodeIndex level) noexcept {
        PriceLevel& lv = levels_[level];
        if (lv.prev_level == kNullNode) {
            side_head(side) = lv.next_level;
        } else {
            levels_[lv.prev_level].next_level = lv.next_level;
        }
        if (lv.next_level != kNullNode) {
            levels_[lv.next_level].prev_level = lv.prev_level;
        }
        levels_.release(level);
    }

    // Insert order `node` into `level`'s FIFO list at its sorted (seq,
    // order_id) position.
    void insert_into_level(NodeIndex level, NodeIndex node) noexcept {
        PriceLevel& lv = levels_[level];
        RestingOrder& ro = orders_[node];

        // Most insertions append at the tail (monotonic seq); scan backward
        // from the tail to find the correct position for out-of-order arrivals.
        NodeIndex cur = lv.tail;
        while (cur != kNullNode &&
               has_priority_over(ro.seq, ro.order_id, orders_[cur].seq,
                                 orders_[cur].order_id)) {
            cur = orders_[cur].prev;  // `node` ranks ahead of `cur`.
        }

        if (cur == kNullNode) {
            // Becomes the new head (highest priority).
            ro.prev = kNullNode;
            ro.next = lv.head;
            if (lv.head != kNullNode) {
                orders_[lv.head].prev = node;
            }
            lv.head = node;
            if (lv.tail == kNullNode) {
                lv.tail = node;
            }
        } else {
            // Insert immediately after `cur`.
            ro.prev = cur;
            ro.next = orders_[cur].next;
            if (orders_[cur].next != kNullNode) {
                orders_[orders_[cur].next].prev = node;
            } else {
                lv.tail = node;
            }
            orders_[cur].next = node;
        }
        ++lv.order_count;
    }

    // Unlink order `node` from `level`'s FIFO list (does not release it).
    void unlink_from_level(NodeIndex level, NodeIndex node) noexcept {
        PriceLevel& lv = levels_[level];
        RestingOrder& ro = orders_[node];
        if (ro.prev == kNullNode) {
            lv.head = ro.next;
        } else {
            orders_[ro.prev].next = ro.next;
        }
        if (ro.next == kNullNode) {
            lv.tail = ro.prev;
        } else {
            orders_[ro.next].prev = ro.prev;
        }
        --lv.order_count;
    }

    // Scan a level's FIFO list for an order id; kNullNode when absent.
    NodeIndex find_in_level(NodeIndex level, std::uint64_t order_id) const noexcept {
        for (NodeIndex node = levels_[level].head; node != kNullNode;
             node = orders_[node].next) {
            if (orders_[node].order_id == order_id) {
                return node;
            }
        }
        return kNullNode;
    }

    FreeListPool<RestingOrder, MaxOrders> orders_{};
    FreeListPool<PriceLevel, MaxPriceLevels> levels_{};
    NodeIndex bid_head_ = kNullNode;  // best bid (highest price) at head.
    NodeIndex ask_head_ = kNullNode;  // best ask (lowest price) at head.
};

}  // namespace hme::engine

#endif  // HME_ORDER_BOOK_HPP
