// Unit tests for incoming-order validation and rejection.
//
// These exercise the validation contract of the two-sink
// hme::engine::MatchingEngine::process_new_order overload:
//   * A NewOrder whose quantity is outside [1, 1,000,000] is rejected with
//     RejectReason::InvalidQuantity, the book is preserved, and no trades are
//     emitted.
//   * A NewOrder whose price_ticks is outside [kMinPriceTicks, kMaxPriceTicks]
//     is rejected with RejectReason::InvalidPrice, the book is preserved, and
//     no trades are emitted.
//   * A valid NewOrder is accepted and matched normally (the rejection sink is
//     never invoked).
//   * The Reject event carries the offending order identifier.
//
// Validation runs before any matching, so a rejected order leaves the
// Order_Book completely unchanged (no matching, no resting).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/matching_engine.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::NewOrder;
using hme::Reject;
using hme::RejectReason;
using hme::Side;
using hme::Trade;
using hme::engine::kNullNode;
using hme::engine::MatchingEngine;

namespace {

using Engine = MatchingEngine<64, 64>;

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

// Outcome of submitting an order through the validating overload.
struct Outcome {
    bool accepted = false;
    std::vector<Trade> trades;
    std::vector<Reject> rejects;
};

Outcome submit(Engine& engine, const NewOrder& order) {
    Outcome out;
    out.accepted = engine.process_new_order(
        order, [&](const Trade& t) { out.trades.push_back(t); },
        [&](const Reject& r) { out.rejects.push_back(r); });
    return out;
}

std::uint32_t resting_qty(const Engine& engine, std::uint64_t id) {
    const auto node = engine.book().find_order(id);
    if (node == kNullNode) {
        return 0;
    }
    return engine.book().order_at(node).remaining_qty;
}

}  // namespace

// ---------------------------------------------------------------------------
// Valid orders pass validation and match/rest normally
// ---------------------------------------------------------------------------

TEST_CASE("A valid order at the range boundaries is accepted and rests",
          "[validate][accept]") {
    Engine engine;

    // Minimum legal quantity and price.
    auto out = submit(engine, make_order(1, Side::Buy,
                                         hme::limits::kMinPriceTicks,
                                         hme::limits::kMinEngineQuantity, 1));
    CHECK(out.accepted);
    CHECK(out.rejects.empty());
    CHECK(out.trades.empty());
    CHECK(resting_qty(engine, 1) == hme::limits::kMinEngineQuantity);

    // Maximum legal quantity and price.
    out = submit(engine, make_order(2, Side::Sell,
                                    hme::limits::kMaxPriceTicks,
                                    hme::limits::kMaxEngineQuantity, 2));
    CHECK(out.accepted);
    CHECK(out.rejects.empty());
    CHECK(resting_qty(engine, 2) == hme::limits::kMaxEngineQuantity);
}

TEST_CASE("A valid crossing order still matches through the validating overload",
          "[validate][accept][match]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 100, 5, 1));  // resting ask
    const auto out = submit(engine, make_order(2, Side::Buy, 101, 5, 2));

    CHECK(out.accepted);
    CHECK(out.rejects.empty());
    REQUIRE(out.trades.size() == 1);
    CHECK(out.trades[0].price_ticks == 100);
    CHECK(out.trades[0].quantity == 5);
    CHECK(engine.book().empty());
}

// ---------------------------------------------------------------------------
// Invalid quantity -> InvalidQuantity rejection, book preserved
// ---------------------------------------------------------------------------

TEST_CASE("Zero quantity is rejected with InvalidQuantity and book unchanged",
          "[validate][reject][quantity]") {
    Engine engine;
    submit(engine, make_order(1, Side::Sell, 100, 5, 1));  // pre-existing book

    const auto out = submit(engine, make_order(2, Side::Buy, 100, 0, 2));

    CHECK_FALSE(out.accepted);
    CHECK(out.trades.empty());
    REQUIRE(out.rejects.size() == 1);
    CHECK(out.rejects[0].reason == RejectReason::InvalidQuantity);
    CHECK(out.rejects[0].order_id == 2);

    // Book preserved: the resting ask is untouched, the rejected order did not
    // match or rest.
    CHECK(resting_qty(engine, 1) == 5);
    CHECK(resting_qty(engine, 2) == 0);
    CHECK(engine.book().order_count() == 1);
}

TEST_CASE("Quantity above 1,000,000 is rejected with InvalidQuantity",
          "[validate][reject][quantity]") {
    Engine engine;
    const auto out = submit(
        engine, make_order(7, Side::Buy, 100,
                           hme::limits::kMaxEngineQuantity + 1, 1));

    CHECK_FALSE(out.accepted);
    CHECK(out.trades.empty());
    REQUIRE(out.rejects.size() == 1);
    CHECK(out.rejects[0].reason == RejectReason::InvalidQuantity);
    CHECK(out.rejects[0].order_id == 7);
    CHECK(engine.book().empty());  // nothing rested.
}

// ---------------------------------------------------------------------------
// Invalid price -> InvalidPrice rejection, book preserved
// ---------------------------------------------------------------------------

TEST_CASE("Price of zero ticks is rejected with InvalidPrice and book unchanged",
          "[validate][reject][price]") {
    Engine engine;
    submit(engine, make_order(1, Side::Buy, 100, 5, 1));  // pre-existing book

    const auto out = submit(engine, make_order(2, Side::Sell, 0, 5, 2));

    CHECK_FALSE(out.accepted);
    CHECK(out.trades.empty());
    REQUIRE(out.rejects.size() == 1);
    CHECK(out.rejects[0].reason == RejectReason::InvalidPrice);
    CHECK(out.rejects[0].order_id == 2);

    CHECK(resting_qty(engine, 1) == 5);
    CHECK(resting_qty(engine, 2) == 0);
    CHECK(engine.book().order_count() == 1);
}

TEST_CASE("Price above the maximum tick is rejected with InvalidPrice",
          "[validate][reject][price]") {
    Engine engine;
    const auto out = submit(
        engine, make_order(9, Side::Buy, hme::limits::kMaxPriceTicks + 1, 5, 1));

    CHECK_FALSE(out.accepted);
    CHECK(out.trades.empty());
    REQUIRE(out.rejects.size() == 1);
    CHECK(out.rejects[0].reason == RejectReason::InvalidPrice);
    CHECK(engine.book().empty());
}

// ---------------------------------------------------------------------------
// Rejection never disturbs an order that would otherwise cross
// ---------------------------------------------------------------------------

TEST_CASE("An invalid order that would cross does not match any resting order",
          "[validate][reject][preserve]") {
    Engine engine;
    // A resting ask the invalid buy would otherwise sweep.
    submit(engine, make_order(1, Side::Sell, 100, 10, 1));

    // Crossing price, but quantity is out of range -> must be rejected, no
    // trade, resting ask intact.
    const auto out = submit(
        engine, make_order(2, Side::Buy, 150,
                           hme::limits::kMaxEngineQuantity + 5, 2));

    CHECK_FALSE(out.accepted);
    CHECK(out.trades.empty());
    REQUIRE(out.rejects.size() == 1);
    CHECK(out.rejects[0].reason == RejectReason::InvalidQuantity);
    CHECK(resting_qty(engine, 1) == 10);  // resting ask untouched.
    CHECK(engine.next_exec_seq() == 1);   // no execution sequence consumed.
}

// ---------------------------------------------------------------------------
// validate_new_order helper directly
// ---------------------------------------------------------------------------

TEST_CASE("validate_new_order classifies range violations", "[validate][helper]") {
    CHECK_FALSE(Engine::validate_new_order(
                    make_order(1, Side::Buy, 100, 5, 1))
                    .has_value());

    CHECK(Engine::validate_new_order(make_order(1, Side::Buy, 100, 0, 1))
              .value() == RejectReason::InvalidQuantity);

    CHECK(Engine::validate_new_order(make_order(1, Side::Buy, 0, 5, 1))
              .value() == RejectReason::InvalidPrice);

    // Quantity is checked first: an order violating both reports quantity.
    CHECK(Engine::validate_new_order(make_order(1, Side::Buy, 0, 0, 1))
              .value() == RejectReason::InvalidQuantity);
}
