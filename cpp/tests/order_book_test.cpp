// Unit tests for Order_Book storage with pre-allocated pools.
//
// These verify the storage contract only (matching and the integrity guard are
// tested separately):
//   * Pre-allocated pools with an intrusive free list: fixed capacity, O(1)
//     acquire/release, graceful exhaustion without growth.
//   * Per-side price ordering: bids descending, asks ascending (best at head).
//   * FIFO ordering within a price level by (seq, order_id).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::Side;
using hme::engine::FreeListPool;
using hme::engine::kNullNode;
using hme::engine::NodeIndex;
using hme::engine::OrderBook;
using hme::engine::RestingOrder;

namespace {

// Collect the order ids at a level (or best level of a side) in FIFO order, by
// walking RestingOrder::next from the level head.
template <typename Book>
std::vector<std::uint64_t> fifo_ids(const Book& book, Side side) {
    std::vector<std::uint64_t> ids;
    const auto* level = (side == Side::Buy) ? book.best_bid() : book.best_ask();
    if (level == nullptr) {
        return ids;
    }
    for (NodeIndex node = book.level_head(*level); node != kNullNode;
         node = book.order_at(node).next) {
        ids.push_back(book.order_at(node).order_id);
    }
    return ids;
}

}  // namespace

// ---------------------------------------------------------------------------
// FreeListPool
// ---------------------------------------------------------------------------

TEST_CASE("FreeListPool reserves fixed capacity and starts empty", "[book][pool]") {
    FreeListPool<RestingOrder, 4> pool;
    CHECK(FreeListPool<RestingOrder, 4>::capacity() == 4);
    CHECK(pool.size() == 0);
    CHECK(pool.empty());
    CHECK_FALSE(pool.full());
}

TEST_CASE("FreeListPool acquires up to capacity then signals exhaustion", "[book][pool]") {
    FreeListPool<RestingOrder, 3> pool;
    const NodeIndex a = pool.acquire();
    const NodeIndex b = pool.acquire();
    const NodeIndex c = pool.acquire();
    CHECK(a != kNullNode);
    CHECK(b != kNullNode);
    CHECK(c != kNullNode);
    CHECK(pool.full());
    CHECK(pool.size() == 3);

    // Exhausted -> kNullNode, no growth.
    CHECK(pool.acquire() == kNullNode);
    CHECK(pool.size() == 3);
}

TEST_CASE("FreeListPool reuses released slots", "[book][pool]") {
    FreeListPool<RestingOrder, 2> pool;
    const NodeIndex a = pool.acquire();
    const NodeIndex b = pool.acquire();
    CHECK(pool.full());

    pool.release(a);
    CHECK(pool.size() == 1);
    CHECK_FALSE(pool.full());

    const NodeIndex reused = pool.acquire();  // takes a freed slot.
    CHECK(reused != kNullNode);
    CHECK(pool.full());
    (void)b;
}

// ---------------------------------------------------------------------------
// Price ordering: bids descending, asks ascending
// ---------------------------------------------------------------------------

TEST_CASE("OrderBook keeps the best bid (highest price) at the head", "[book][order]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(/*id=*/1, Side::Buy, /*price=*/100, /*seq=*/1, /*qty=*/5));
    REQUIRE(book.insert_order(2, Side::Buy, 102, 2, 5));
    REQUIRE(book.insert_order(3, Side::Buy, 101, 3, 5));

    REQUIRE(book.best_bid() != nullptr);
    CHECK(book.best_bid()->price_ticks == 102);  // highest bid is best.
    CHECK(book.best_ask() == nullptr);
    CHECK(book.level_count() == 3);
    CHECK(book.order_count() == 3);
}

TEST_CASE("OrderBook keeps the best ask (lowest price) at the head", "[book][order]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(1, Side::Sell, 200, 1, 5));
    REQUIRE(book.insert_order(2, Side::Sell, 198, 2, 5));
    REQUIRE(book.insert_order(3, Side::Sell, 199, 3, 5));

    REQUIRE(book.best_ask() != nullptr);
    CHECK(book.best_ask()->price_ticks == 198);  // lowest ask is best.
    CHECK(book.best_bid() == nullptr);
}

TEST_CASE("OrderBook groups orders at the same price into one level", "[book][order]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Buy, 100, 2, 7));
    CHECK(book.level_count() == 1);
    CHECK(book.order_count() == 2);
    REQUIRE(book.best_bid() != nullptr);
    CHECK(book.best_bid()->order_count == 2);
}

// ---------------------------------------------------------------------------
// FIFO within a level by (seq, order_id)
// ---------------------------------------------------------------------------

TEST_CASE("Within a level orders are ordered by seq", "[book][fifo]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(10, Side::Buy, 100, /*seq=*/3, 5));
    REQUIRE(book.insert_order(11, Side::Buy, 100, /*seq=*/1, 5));
    REQUIRE(book.insert_order(12, Side::Buy, 100, /*seq=*/2, 5));

    // Earliest seq first regardless of insertion order.
    CHECK(fifo_ids(book, Side::Buy) == std::vector<std::uint64_t>{11, 12, 10});
    CHECK(book.front_order(Side::Buy)->order_id == 11);
}

TEST_CASE("Equal seq ties break by lower order_id", "[book][fifo]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(/*id=*/30, Side::Sell, 200, /*seq=*/5, 5));
    REQUIRE(book.insert_order(/*id=*/10, Side::Sell, 200, /*seq=*/5, 5));
    REQUIRE(book.insert_order(/*id=*/20, Side::Sell, 200, /*seq=*/5, 5));

    CHECK(fifo_ids(book, Side::Sell) == std::vector<std::uint64_t>{10, 20, 30});
}

TEST_CASE("Monotonic arrivals append at the tail", "[book][fifo]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Buy, 100, 2, 5));
    REQUIRE(book.insert_order(3, Side::Buy, 100, 3, 5));
    CHECK(fifo_ids(book, Side::Buy) == std::vector<std::uint64_t>{1, 2, 3});
}

// ---------------------------------------------------------------------------
// Removal
// ---------------------------------------------------------------------------

TEST_CASE("Removing the only order at a level drops the level", "[book][remove]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    const NodeIndex node = book.find_order(1);
    REQUIRE(node != kNullNode);

    book.remove_order(node);
    CHECK(book.empty());
    CHECK(book.level_count() == 0);
    CHECK(book.best_bid() == nullptr);
    CHECK(book.find_order(1) == kNullNode);
}

TEST_CASE("Removing the head re-links the level FIFO", "[book][remove]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Buy, 100, 2, 5));
    REQUIRE(book.insert_order(3, Side::Buy, 100, 3, 5));

    book.remove_order(book.find_order(1));
    CHECK(fifo_ids(book, Side::Buy) == std::vector<std::uint64_t>{2, 3});
    CHECK(book.front_order(Side::Buy)->order_id == 2);

    book.remove_order(book.find_order(3));  // remove the tail.
    CHECK(fifo_ids(book, Side::Buy) == std::vector<std::uint64_t>{2});
}

TEST_CASE("Removing a middle level re-links the side list", "[book][remove]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Buy, 102, 2, 5));
    REQUIRE(book.insert_order(3, Side::Buy, 101, 3, 5));

    book.remove_order(book.find_order(3));  // drops the 101 level.
    CHECK(book.level_count() == 2);
    REQUIRE(book.best_bid() != nullptr);
    CHECK(book.best_bid()->price_ticks == 102);

    // Walk the remaining levels via level_after: 102 then 100 (descending).
    std::vector<std::uint64_t> prices;
    for (const auto* lvl = book.best_bid(); lvl != nullptr;
         lvl = book.level_after(*lvl)) {
        prices.push_back(lvl->price_ticks);
    }
    CHECK(prices == std::vector<std::uint64_t>{102, 100});
}

// ---------------------------------------------------------------------------
// Pool exhaustion -> graceful rejection
// ---------------------------------------------------------------------------

TEST_CASE("Order pool exhaustion rejects without growth", "[book][capacity]") {
    OrderBook<2, 8> book;  // only 2 order slots.
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Buy, 101, 2, 5));
    CHECK(book.order_count() == 2);

    // Third insert has no slot -> rejected, book unchanged.
    CHECK_FALSE(book.insert_order(3, Side::Buy, 102, 3, 5));
    CHECK(book.order_count() == 2);
}

TEST_CASE("Level pool exhaustion rejects a new price without growth", "[book][capacity]") {
    OrderBook<8, 2> book;  // only 2 level slots.
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Buy, 101, 2, 5));
    CHECK(book.level_count() == 2);

    // A new price needs a third level slot -> rejected.
    CHECK_FALSE(book.insert_order(3, Side::Buy, 102, 3, 5));
    CHECK(book.level_count() == 2);
    CHECK(book.order_count() == 2);

    // But another order at an existing price still fits (no new level).
    CHECK(book.insert_order(4, Side::Buy, 100, 4, 5));
    CHECK(book.order_count() == 3);
}

TEST_CASE("Bids and asks coexist independently", "[book][order]") {
    OrderBook<64, 64> book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Sell, 105, 2, 5));
    REQUIRE(book.best_bid() != nullptr);
    REQUIRE(book.best_ask() != nullptr);
    CHECK(book.best_bid()->price_ticks == 100);
    CHECK(book.best_ask()->price_ticks == 105);
    CHECK(book.level_count() == 2);
}
