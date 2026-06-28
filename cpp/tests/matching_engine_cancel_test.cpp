// Unit tests for CancelOrder processing.
//
// These exercise the cancellation contract of
// hme::engine::MatchingEngine::process_cancel_order:
//   * A Cancel_Request for a resting order removes it from the Order_Book and
//     emits exactly one Ack with AckKind::Cancelled carrying the cancelled
//     order identifier.
//   * A cancelled identifier is excluded from all subsequent matching - a later
//     order that would have crossed it does not match.
//   * A Cancel_Request for an identifier not present in the book leaves the
//     Order_Book unchanged and emits exactly one Reject. The reason
//     distinguishes an id never seen (RejectReason::OrderNotFound) from one that
//     was once resting but has since left the book - fully filled or already
//     cancelled (RejectReason::NoLongerResting).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/matching_engine.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::Ack;
using hme::AckKind;
using hme::CancelOrder;
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

// Rest a NewOrder in the book (no crossing counterpart expected here).
void rest_order(Engine& engine, const NewOrder& order) {
    engine.process_new_order(order, [](const Trade&) {});
}

// Outcome of submitting a CancelOrder through the engine.
struct CancelOutcome {
    bool removed = false;
    std::vector<Ack> acks;
    std::vector<Reject> rejects;
};

CancelOutcome cancel(Engine& engine, std::uint64_t id) {
    CancelOutcome out;
    CancelOrder c;
    c.order_id = id;
    out.removed = engine.process_cancel_order(
        c, [&](const Ack& a) { out.acks.push_back(a); },
        [&](const Reject& r) { out.rejects.push_back(r); });
    return out;
}

bool is_resting(const Engine& engine, std::uint64_t id) {
    return engine.book().find_order(id) != kNullNode;
}

}  // namespace

// ---------------------------------------------------------------------------
// Cancelling a resting order removes it and acks
// ---------------------------------------------------------------------------

TEST_CASE("Cancelling a resting order removes it and emits a Cancelled ack",
          "[cancel][ack]") {
    Engine engine;
    rest_order(engine, make_order(1, Side::Buy, 100, 5, 1));
    REQUIRE(is_resting(engine, 1));

    const auto out = cancel(engine, 1);

    CHECK(out.removed);
    CHECK(out.rejects.empty());
    REQUIRE(out.acks.size() == 1);
    CHECK(out.acks[0].order_id == 1);
    CHECK(out.acks[0].kind == AckKind::Cancelled);

    // Order no longer in the book; the book is now empty.
    CHECK_FALSE(is_resting(engine, 1));
    CHECK(engine.book().empty());
}

TEST_CASE("Cancelling one of several resting orders leaves the rest intact",
          "[cancel][ack]") {
    Engine engine;
    rest_order(engine, make_order(1, Side::Buy, 100, 5, 1));
    rest_order(engine, make_order(2, Side::Buy, 100, 5, 2));
    rest_order(engine, make_order(3, Side::Sell, 110, 5, 3));

    const auto out = cancel(engine, 2);

    CHECK(out.removed);
    REQUIRE(out.acks.size() == 1);
    CHECK(out.acks[0].order_id == 2);
    CHECK(out.acks[0].kind == AckKind::Cancelled);

    CHECK_FALSE(is_resting(engine, 2));
    CHECK(is_resting(engine, 1));
    CHECK(is_resting(engine, 3));
    CHECK(engine.book().order_count() == 2);
}

// ---------------------------------------------------------------------------
// A cancelled order is excluded from subsequent matching
// ---------------------------------------------------------------------------

TEST_CASE("A cancelled resting order does not match a later crossing order",
          "[cancel][exclude]") {
    Engine engine;
    // Resting ask at 100 that a later buy at 105 would otherwise sweep.
    rest_order(engine, make_order(1, Side::Sell, 100, 5, 1));

    const auto out = cancel(engine, 1);
    CHECK(out.removed);
    REQUIRE(out.acks.size() == 1);

    // A crossing buy now finds nothing to match against and simply rests.
    std::vector<Trade> trades;
    engine.process_new_order(make_order(2, Side::Buy, 105, 5, 2),
                             [&](const Trade& t) { trades.push_back(t); });

    CHECK(trades.empty());                 // cancelled ask excluded from matching.
    CHECK(is_resting(engine, 2));          // the buy rests instead.
    CHECK(engine.book().best_ask() == nullptr);
}

// ---------------------------------------------------------------------------
// Cancelling an absent order is rejected without effect
// ---------------------------------------------------------------------------

TEST_CASE("Cancelling an unknown order is rejected with OrderNotFound",
          "[cancel][reject]") {
    Engine engine;
    rest_order(engine, make_order(1, Side::Buy, 100, 5, 1));

    const auto out = cancel(engine, 999);  // never seen.

    CHECK_FALSE(out.removed);
    CHECK(out.acks.empty());
    REQUIRE(out.rejects.size() == 1);
    CHECK(out.rejects[0].order_id == 999);
    CHECK(out.rejects[0].reason == RejectReason::OrderNotFound);

    // Book unchanged.
    CHECK(is_resting(engine, 1));
    CHECK(engine.book().order_count() == 1);
}

TEST_CASE("Cancelling an already fully-filled order is rejected without effect",
          "[cancel][reject]") {
    Engine engine;
    rest_order(engine, make_order(1, Side::Sell, 100, 5, 1));
    // Fully fill order 1 with a crossing buy.
    std::vector<Trade> trades;
    engine.process_new_order(make_order(2, Side::Buy, 100, 5, 2),
                             [&](const Trade& t) { trades.push_back(t); });
    REQUIRE(trades.size() == 1);
    REQUIRE_FALSE(is_resting(engine, 1));

    const auto out = cancel(engine, 1);  // no longer resting (once resting).

    CHECK_FALSE(out.removed);
    CHECK(out.acks.empty());
    REQUIRE(out.rejects.size() == 1);
    CHECK(out.rejects[0].order_id == 1);
    CHECK(out.rejects[0].reason == RejectReason::NoLongerResting);
    CHECK(engine.book().empty());
}

TEST_CASE("Cancelling an already-cancelled order is rejected without effect",
          "[cancel][reject]") {
    Engine engine;
    rest_order(engine, make_order(1, Side::Buy, 100, 5, 1));

    const auto first = cancel(engine, 1);
    REQUIRE(first.removed);
    REQUIRE(first.acks.size() == 1);

    const auto second = cancel(engine, 1);  // already cancelled (once resting).

    CHECK_FALSE(second.removed);
    CHECK(second.acks.empty());
    REQUIRE(second.rejects.size() == 1);
    CHECK(second.rejects[0].order_id == 1);
    CHECK(second.rejects[0].reason == RejectReason::NoLongerResting);
    CHECK(engine.book().empty());
}
