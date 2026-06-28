// Unit tests for NewOrder matching with price-time priority.
//
// These exercise the matching contract of hme::engine::MatchingEngine against
// the pre-allocated Order_Book storage:
//   * Selection by best price, then earliest seq, then lowest order id.
//   * Trade executed at the resting order's limit price with quantity = min of
//     the two remaining quantities.
//   * Continue until filled or no eligible counterpart.
//   * Fully filled resting orders are removed.
//   * Any incoming remainder rests at its own limit price.
//   * Trade events carry exec_seq, price, quantity, incoming id, resting id,
//     with a monotonic exec_seq.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/matching_engine.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::NewOrder;
using hme::Side;
using hme::Trade;
using hme::engine::kNullNode;
using hme::engine::MatchingEngine;

namespace {

using Engine = MatchingEngine<64, 64>;

// A NewOrder builder for readability.
NewOrder make_order(std::uint64_t id, Side side, std::uint64_t price,
                    std::uint32_t qty, std::uint64_t seq) {
    NewOrder o;
    o.order_id = id;
    o.side = side;
    o.price_ticks = price;
    o.quantity = qty;
    o.seq = seq;
    return o;
}

// Run a NewOrder through the engine, collecting emitted trades.
std::vector<Trade> submit(Engine& engine, const NewOrder& order) {
    std::vector<Trade> trades;
    engine.process_new_order(order,
                             [&](const Trade& t) { trades.push_back(t); });
    return trades;
}

// Remaining quantity of a resting order by id, or 0 if not resting.
std::uint32_t resting_qty(const Engine& engine, std::uint64_t id) {
    const auto node = engine.book().find_order(id);
    if (node == kNullNode) {
        return 0;
    }
    return engine.book().order_at(node).remaining_qty;
}

}  // namespace

// ---------------------------------------------------------------------------
// No match -> the order rests
// ---------------------------------------------------------------------------

TEST_CASE("Incoming order with no eligible counterpart rests in the book",
          "[match][rest]") {
    Engine engine;
    // Empty book: a buy at 100 cannot match and rests.
    const auto trades = submit(engine, make_order(1, Side::Buy, 100, 5, 1));
    CHECK(trades.empty());
    CHECK(resting_qty(engine, 1) == 5);
    REQUIRE(engine.book().best_bid() != nullptr);
    CHECK(engine.book().best_bid()->price_ticks == 100);
}

TEST_CASE("Non-crossing prices do not match and both rest", "[match][rest]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 105, 5, 1));  // ask at 105
    const auto trades = submit(engine, make_order(2, Side::Buy, 100, 5, 2));  // bid 100
    CHECK(trades.empty());
    CHECK(resting_qty(engine, 1) == 5);
    CHECK(resting_qty(engine, 2) == 5);
}

// ---------------------------------------------------------------------------
// Trade pricing and sizing
// ---------------------------------------------------------------------------

TEST_CASE("Exact-fill trade executes at the resting price and removes both",
          "[match][fill]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 100, 5, 1));  // resting ask at 100
    const auto trades = submit(engine, make_order(2, Side::Buy, 101, 5, 2));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].price_ticks == 100);  // resting limit price, not the buy's 101.
    CHECK(trades[0].quantity == 5);
    CHECK(trades[0].incoming_id == 2);
    CHECK(trades[0].resting_id == 1);

    // Both fully filled -> neither rests, book empty.
    CHECK(resting_qty(engine, 1) == 0);
    CHECK(resting_qty(engine, 2) == 0);
    CHECK(engine.book().empty());
}

TEST_CASE("Incoming larger than resting: resting removed, remainder rests",
          "[match][partial]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 100, 3, 1));  // resting ask qty 3
    const auto trades = submit(engine, make_order(2, Side::Buy, 100, 5, 2));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].quantity == 3);  // min(5, 3)
    CHECK(trades[0].price_ticks == 100);

    CHECK(resting_qty(engine, 1) == 0);  // resting fully filled, removed.
    CHECK(resting_qty(engine, 2) == 2);  // incoming remainder rests as a bid.
    REQUIRE(engine.book().best_bid() != nullptr);
    CHECK(engine.book().best_bid()->price_ticks == 100);
    CHECK(engine.book().best_ask() == nullptr);
}

TEST_CASE("Incoming smaller than resting: incoming filled, resting reduced",
          "[match][partial]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 100, 10, 1));
    const auto trades = submit(engine, make_order(2, Side::Buy, 100, 4, 2));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].quantity == 4);
    CHECK(resting_qty(engine, 1) == 6);  // resting partially filled, still rests.
    CHECK(resting_qty(engine, 2) == 0);  // incoming fully filled, does not rest.
}

// ---------------------------------------------------------------------------
// Continue across multiple counterparts
// ---------------------------------------------------------------------------

TEST_CASE("Incoming sweeps several resting orders until filled",
          "[match][sweep]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 100, 2, 1));
    submit(engine, make_order(2, Side::Sell, 100, 2, 2));
    submit(engine, make_order(3, Side::Sell, 100, 2, 3));

    const auto trades = submit(engine, make_order(9, Side::Buy, 100, 5, 4));

    // Fills 2 + 2 + 1 = 5 across three resting orders.
    REQUIRE(trades.size() == 3);
    CHECK(trades[0].resting_id == 1);
    CHECK(trades[0].quantity == 2);
    CHECK(trades[1].resting_id == 2);
    CHECK(trades[1].quantity == 2);
    CHECK(trades[2].resting_id == 3);
    CHECK(trades[2].quantity == 1);

    CHECK(resting_qty(engine, 1) == 0);
    CHECK(resting_qty(engine, 2) == 0);
    CHECK(resting_qty(engine, 3) == 1);  // last one partially filled.
    CHECK(resting_qty(engine, 9) == 0);  // incoming fully filled.
}

// ---------------------------------------------------------------------------
// Price priority across levels
// ---------------------------------------------------------------------------

TEST_CASE("Buy matches the lowest-priced ask first", "[match][price-priority]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 102, 5, 1));
    submit(engine, make_order(2, Side::Sell, 100, 5, 2));  // best (lowest) ask
    submit(engine, make_order(3, Side::Sell, 101, 5, 3));

    const auto trades = submit(engine, make_order(9, Side::Buy, 101, 5, 4));

    // Crosses asks at 100 and 101 only (102 is above the buy limit).
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].resting_id == 2);   // lowest ask consumed first.
    CHECK(trades[0].price_ticks == 100);
    CHECK(resting_qty(engine, 2) == 0);
    CHECK(resting_qty(engine, 3) == 5);  // 101 untouched (buy already filled).
    CHECK(resting_qty(engine, 1) == 5);  // 102 ineligible.
}

TEST_CASE("Sell matches the highest-priced bid first", "[match][price-priority]") {
    Engine engine;
    submit(engine, make_order(1, Side::Buy, 98, 5, 1));
    submit(engine, make_order(2, Side::Buy, 100, 5, 2));  // best (highest) bid
    submit(engine, make_order(3, Side::Buy, 99, 5, 3));

    const auto trades = submit(engine, make_order(9, Side::Sell, 99, 5, 4));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].resting_id == 2);   // highest bid consumed first.
    CHECK(trades[0].price_ticks == 100);
    CHECK(resting_qty(engine, 2) == 0);
    CHECK(resting_qty(engine, 1) == 5);  // 98 below the sell limit -> ineligible.
}

// ---------------------------------------------------------------------------
// Time priority within a level
// ---------------------------------------------------------------------------

TEST_CASE("Within a price level the earliest seq matches first",
          "[match][time-priority]") {
    Engine engine;
    submit(engine, make_order(10, Side::Sell, 100, 5, /*seq=*/3));
    submit(engine, make_order(11, Side::Sell, 100, 5, /*seq=*/1));
    submit(engine, make_order(12, Side::Sell, 100, 5, /*seq=*/2));

    const auto trades = submit(engine, make_order(9, Side::Buy, 100, 5, 4));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].resting_id == 11);  // earliest seq (1) first.
}

TEST_CASE("Equal seq ties break by the lower order id", "[match][time-priority]") {
    Engine engine;
    submit(engine, make_order(30, Side::Sell, 100, 5, /*seq=*/5));
    submit(engine, make_order(10, Side::Sell, 100, 5, /*seq=*/5));
    submit(engine, make_order(20, Side::Sell, 100, 5, /*seq=*/5));

    const auto trades = submit(engine, make_order(9, Side::Buy, 100, 5, 6));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].resting_id == 10);  // lowest id among equal seq.
}

// ---------------------------------------------------------------------------
// Trade event completeness and monotonic exec_seq
// ---------------------------------------------------------------------------

TEST_CASE("exec_seq is monotonic and trade fields are fully populated",
          "[match][event]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 100, 2, 1));
    submit(engine, make_order(2, Side::Sell, 100, 2, 2));

    const std::uint64_t first = engine.next_exec_seq();
    const auto trades = submit(engine, make_order(9, Side::Buy, 100, 4, 3));

    REQUIRE(trades.size() == 2);
    CHECK(trades[0].exec_seq == first);
    CHECK(trades[1].exec_seq == first + 1);  // strictly increasing.
    CHECK(engine.next_exec_seq() == first + 2);

    for (const auto& t : trades) {
        CHECK(t.incoming_id == 9);
        CHECK(t.quantity > 0);
        CHECK(t.price_ticks == 100);
    }
    CHECK(trades[0].resting_id == 1);
    CHECK(trades[1].resting_id == 2);
}

// ---------------------------------------------------------------------------
// Quantity conservation
// ---------------------------------------------------------------------------

TEST_CASE("Incoming quantity equals traded plus rested", "[match][conservation]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 100, 3, 1));

    const auto trades = submit(engine, make_order(2, Side::Buy, 100, 7, 2));

    std::uint32_t traded = 0;
    for (const auto& t : trades) {
        traded += t.quantity;
    }
    const std::uint32_t rested = resting_qty(engine, 2);
    CHECK(traded + rested == 7);  // 3 traded + 4 rested.
    CHECK(traded == 3);
    CHECK(rested == 4);
}
