// Unit tests for the Matching_Engine FIFO processing loop.
//
// These exercise the loop contract of hme::engine::EngineProcessor:
//   * process_next() dequeues exactly one ingress message in FIFO order,
//     dispatches it, and emits resulting events to the egress ring.
//   * A NewOrder is validated then matched; a CancelOrder removes + Acks or
//     rejects; events land on the egress ring in order.
//   * Continue-on-error: an invalid message yields exactly one error event and
//     the remaining messages are still processed in arrival order.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <variant>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/engine_processor.hpp"
#include "hme/wire_protocol.hpp"

using hme::Ack;
using hme::AckKind;
using hme::BinaryMessage;
using hme::CancelOrder;
using hme::NewOrder;
using hme::Reject;
using hme::RejectReason;
using hme::Side;
using hme::Trade;
using hme::engine::EngineProcessor;

namespace {

using Processor = EngineProcessor<64, 64, 64, 256>;

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

// Drain every outbound event currently on the egress ring, in FIFO order.
std::vector<BinaryMessage> drain(Processor& proc) {
    std::vector<BinaryMessage> events;
    BinaryMessage ev;
    while (proc.next_event(ev)) {
        events.push_back(ev);
    }
    return events;
}

}  // namespace

// ---------------------------------------------------------------------------
// Single-message dispatch
// ---------------------------------------------------------------------------

TEST_CASE("process_next on an empty ingress ring does nothing",
          "[loop][empty]") {
    Processor proc;
    CHECK_FALSE(proc.process_next());
    CHECK(proc.processed_count() == 0);
    CHECK(drain(proc).empty());
}

TEST_CASE("A resting NewOrder emits a trailing Accepted ack and rests",
          "[loop][rest]") {
    Processor proc;
    REQUIRE(proc.submit(make_order(1, Side::Buy, 100, 5, 1)));

    REQUIRE(proc.process_next());
    CHECK(proc.processed_count() == 1);

    // No counterpart -> no Trade, but an accepted NewOrder terminates with
    // exactly one Ack(Accepted) carrying its id.
    const auto events = drain(proc);
    REQUIRE(events.size() == 1);
    const Ack* ack = std::get_if<Ack>(&events[0]);
    REQUIRE(ack != nullptr);
    CHECK(ack->order_id == 1);
    CHECK(ack->kind == AckKind::Accepted);
    REQUIRE(proc.engine().book().best_bid() != nullptr);
    CHECK(proc.engine().book().best_bid()->price_ticks == 100);
}

TEST_CASE("A crossing NewOrder emits a Trade event then an Accepted ack",
          "[loop][trade]") {
    Processor proc;
    REQUIRE(proc.submit(make_order(1, Side::Sell, 100, 5, 1)));
    REQUIRE(proc.submit(make_order(2, Side::Buy, 101, 5, 2)));

    REQUIRE(proc.process_next());  // rest the ask -> Accepted(1)
    REQUIRE(proc.process_next());  // cross with the bid -> Trade, Accepted(2)
    CHECK(proc.processed_count() == 2);

    // In order: Accepted(1) for the rested ask, then the Trade, then
    // Accepted(2) terminating the crossing buy.
    const auto events = drain(proc);
    REQUIRE(events.size() == 3);
    const Ack* rest_ack = std::get_if<Ack>(&events[0]);
    REQUIRE(rest_ack != nullptr);
    CHECK(rest_ack->order_id == 1);
    CHECK(rest_ack->kind == AckKind::Accepted);
    const Trade* trade = std::get_if<Trade>(&events[1]);
    REQUIRE(trade != nullptr);
    CHECK(trade->price_ticks == 100);  // resting limit price.
    CHECK(trade->quantity == 5);
    CHECK(trade->incoming_id == 2);
    CHECK(trade->resting_id == 1);
    const Ack* cross_ack = std::get_if<Ack>(&events[2]);
    REQUIRE(cross_ack != nullptr);
    CHECK(cross_ack->order_id == 2);
    CHECK(cross_ack->kind == AckKind::Accepted);
    CHECK(proc.error_event_count() == 0);
}

// ---------------------------------------------------------------------------
// Cancellation dispatch
// ---------------------------------------------------------------------------

TEST_CASE("A CancelOrder for a resting order emits a Cancelled Ack",
          "[loop][cancel]") {
    Processor proc;
    REQUIRE(proc.submit(make_order(7, Side::Buy, 100, 5, 1)));
    REQUIRE(proc.submit(CancelOrder{7}));

    REQUIRE(proc.process_next());  // rest order 7 -> Accepted(7)
    REQUIRE(proc.process_next());  // cancel order 7 -> Cancelled(7)

    // The accepted rest emits Ack(Accepted), then the cancel emits
    // Ack(Cancelled), in that order.
    const auto events = drain(proc);
    REQUIRE(events.size() == 2);
    const Ack* accepted = std::get_if<Ack>(&events[0]);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->order_id == 7);
    CHECK(accepted->kind == AckKind::Accepted);
    const Ack* ack = std::get_if<Ack>(&events[1]);
    REQUIRE(ack != nullptr);
    CHECK(ack->order_id == 7);
    CHECK(ack->kind == AckKind::Cancelled);
    CHECK(proc.engine().book().empty());
    // A well-formed cancel that succeeds is not an invalid-message error.
    CHECK(proc.error_event_count() == 0);
}

TEST_CASE("A CancelOrder for an unknown order emits one cancellation Reject",
          "[loop][cancel]") {
    Processor proc;
    REQUIRE(proc.submit(CancelOrder{999}));

    REQUIRE(proc.process_next());

    const auto events = drain(proc);
    REQUIRE(events.size() == 1);
    const Reject* reject = std::get_if<Reject>(&events[0]);
    REQUIRE(reject != nullptr);
    CHECK(reject->order_id == 999);
    CHECK(reject->reason == RejectReason::OrderNotFound);
    // A not-resting cancel is a normal outcome of a valid command.
    CHECK(proc.error_event_count() == 0);
}

// ---------------------------------------------------------------------------
// Continue-on-error: one error event per invalid message, order preserved
// ---------------------------------------------------------------------------

TEST_CASE("An invalid NewOrder emits exactly one Reject and the book is preserved",
          "[loop][error]") {
    Processor proc;
    // Quantity 0 is below the engine's permitted range -> validation failure.
    REQUIRE(proc.submit(make_order(1, Side::Buy, 100, 0, 1)));

    REQUIRE(proc.process_next());

    const auto events = drain(proc);
    REQUIRE(events.size() == 1);
    const Reject* reject = std::get_if<Reject>(&events[0]);
    REQUIRE(reject != nullptr);
    CHECK(reject->order_id == 1);
    CHECK(reject->reason == RejectReason::InvalidQuantity);
    CHECK(proc.error_event_count() == 1);
    CHECK(proc.engine().book().empty());  // book unchanged.
}

TEST_CASE("Out-of-place outbound variant on ingress yields one error event",
          "[loop][error]") {
    Processor proc;
    Ack stray;  // an Ack is an outbound event, never a valid inbound command.
    stray.order_id = 42;
    stray.kind = AckKind::Accepted;
    REQUIRE(proc.submit(BinaryMessage{stray}));

    REQUIRE(proc.process_next());

    const auto events = drain(proc);
    REQUIRE(events.size() == 1);
    const Reject* reject = std::get_if<Reject>(&events[0]);
    REQUIRE(reject != nullptr);
    CHECK(reject->order_id == 42);  // best-effort id carried through.
    CHECK(proc.error_event_count() == 1);
}

TEST_CASE("Processing continues in FIFO order after an invalid message",
          "[loop][error][fifo]") {
    Processor proc;
    // Interleave a valid resting order, an invalid order, and a crossing order.
    REQUIRE(proc.submit(make_order(1, Side::Sell, 100, 5, 1)));  // valid, rests
    REQUIRE(proc.submit(make_order(2, Side::Buy, 100, 0, 2)));   // invalid qty
    REQUIRE(proc.submit(make_order(3, Side::Buy, 100, 5, 3)));   // valid, crosses

    CHECK(proc.process_all() == 3);
    CHECK(proc.processed_count() == 3);
    CHECK(proc.error_event_count() == 1);  // exactly one for the invalid order.

    const auto events = drain(proc);
    // Expect, in order: Accepted(1) for the rested ask, Reject (order 2),
    // Trade (order 3 vs resting 1), then Accepted(3) terminating order 3.
    REQUIRE(events.size() == 4);
    const Ack* accepted1 = std::get_if<Ack>(&events[0]);
    REQUIRE(accepted1 != nullptr);
    CHECK(accepted1->order_id == 1);
    CHECK(accepted1->kind == AckKind::Accepted);

    const Reject* reject = std::get_if<Reject>(&events[1]);
    REQUIRE(reject != nullptr);
    CHECK(reject->order_id == 2);
    CHECK(reject->reason == RejectReason::InvalidQuantity);

    const Trade* trade = std::get_if<Trade>(&events[2]);
    REQUIRE(trade != nullptr);
    CHECK(trade->incoming_id == 3);
    CHECK(trade->resting_id == 1);
    CHECK(trade->quantity == 5);

    const Ack* accepted3 = std::get_if<Ack>(&events[3]);
    REQUIRE(accepted3 != nullptr);
    CHECK(accepted3->order_id == 3);
    CHECK(accepted3->kind == AckKind::Accepted);

    // The invalid message did not disturb matching of the surrounding orders.
    CHECK(proc.engine().book().empty());
}

// ---------------------------------------------------------------------------
// FIFO dequeue order
// ---------------------------------------------------------------------------

TEST_CASE("Messages are dequeued strictly in arrival order", "[loop][fifo]") {
    Processor proc;
    // Three asks that the incoming buy sweeps; FIFO dequeue means they rest in
    // submission order and the resulting trades reference them in that order.
    REQUIRE(proc.submit(make_order(10, Side::Sell, 100, 1, 1)));
    REQUIRE(proc.submit(make_order(11, Side::Sell, 100, 1, 2)));
    REQUIRE(proc.submit(make_order(12, Side::Sell, 100, 1, 3)));
    REQUIRE(proc.submit(make_order(99, Side::Buy, 100, 3, 4)));

    CHECK(proc.process_all() == 4);

    // Three rested asks each emit Ack(Accepted), then the sweeping buy emits a
    // Trade per consumed ask (in FIFO order) and its own terminating
    // Ack(Accepted): Accepted(10), Accepted(11), Accepted(12), Trade(10),
    // Trade(11), Trade(12), Accepted(99).
    const auto events = drain(proc);
    REQUIRE(events.size() == 7);
    CHECK(std::get<Ack>(events[0]).order_id == 10);
    CHECK(std::get<Ack>(events[1]).order_id == 11);
    CHECK(std::get<Ack>(events[2]).order_id == 12);
    CHECK(std::get<Trade>(events[3]).resting_id == 10);
    CHECK(std::get<Trade>(events[4]).resting_id == 11);
    CHECK(std::get<Trade>(events[5]).resting_id == 12);
    CHECK(std::get<Ack>(events[6]).order_id == 99);
    CHECK(std::get<Ack>(events[6]).kind == AckKind::Accepted);
}

// ---------------------------------------------------------------------------
// Back-pressure on a full ingress ring
// ---------------------------------------------------------------------------

TEST_CASE("submit reports back-pressure when the ingress ring is full",
          "[loop][backpressure]") {
    EngineProcessor<8, 8, 2, 8> small;
    REQUIRE(small.submit(make_order(1, Side::Buy, 100, 1, 1)));
    REQUIRE(small.submit(make_order(2, Side::Buy, 100, 1, 2)));
    // Ingress capacity is 2; the third submit is refused without disturbing the
    // first two.
    CHECK_FALSE(small.submit(make_order(3, Side::Buy, 100, 1, 3)));

    CHECK(small.process_all() == 2);  // only the two accepted orders process.
}
