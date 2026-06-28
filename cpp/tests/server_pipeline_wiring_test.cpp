// Wiring smoke tests for the assembled in-process data path.
//
// These tests verify that ServerPipeline correctly COMPOSES the already-tested
// components into the end-to-end path:
//
//     client bytes -> Network_Server framing -> ingress ring ->
//     Matching_Engine -> egress ring -> encoded response bytes -> client
//
// They drive framed inbound Binary_Message bytes into the pipeline and assert
// the expected response Binary_Message bytes come back out, exercising the
// seams between framing, the rings, the engine, and response encoding. The
// per-component behavior (matching correctness, framing reassembly, codec round
// trips, lifecycle) is covered by the dedicated unit / property tests; this
// file only checks that the assembly is wired up.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/connection_registry.hpp"
#include "hme/network.hpp"
#include "hme/server_pipeline.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;
using hme::network::AcceptResult;
using hme::network::FrameStatus;
using hme::network::ReadinessPoller;
using hme::network::SocketHandle;
using hme::network::TeardownReason;

namespace {

// A no-op readiness poller: the data-path wiring tests do not need to model OS
// socket resources (that is covered by the lifecycle integration tests), only
// to satisfy the ConnectionRegistry the pipeline owns.
struct NoopPoller : ReadinessPoller {
    void register_readiness(SocketHandle) override {}
    void deregister_readiness(SocketHandle) override {}
    void close_socket(SocketHandle) override {}
};

// Append the wire-encoding of `msg` to `out`.
void append_encoded(std::vector<std::uint8_t>& out, const BinaryMessage& msg) {
    const std::size_t at = out.size();
    out.resize(at + encoded_len_of(msg));
    auto res = encode(
        msg, std::span<std::uint8_t>{out.data() + at, encoded_len_of(msg)});
    REQUIRE(res.is_ok());
}

// An outbound sink that decodes each response frame the pipeline emits, so
// tests can assert on the response Binary_Messages and their originating
// socket -- mirroring what the Rust Gateway does at the client edge.
struct ResponseCollector {
    std::vector<std::pair<SocketHandle, BinaryMessage>> responses;

    void operator()(SocketHandle fd, std::span<const std::uint8_t> frame) {
        auto decoded = decode(frame);
        REQUIRE(decoded.is_ok());
        responses.emplace_back(fd, decoded.value());
    }
};

// A small, explicitly-sized pipeline keeps the test cheap while still composing
// the real components.
using TestPipeline = server::ServerPipeline</*MaxOrders=*/256,
                                            /*MaxPriceLevels=*/64,
                                            /*IngressCapacity=*/64,
                                            /*EgressCapacity=*/64>;

}  // namespace

TEST_CASE("wiring: a crossing order flows framed-in -> engine -> framed-out",
          "[integration][wiring]") {
    // The core data path: a resting sell followed by a crossing buy produces a
    // Trade that is encoded and returned to the originating connection.
    NoopPoller poller;
    TestPipeline pipeline(poller);
    REQUIRE(pipeline.initialize());
    REQUIRE(pipeline.operational());

    const SocketHandle fd = 7;
    REQUIRE(pipeline.accept(fd) == AcceptResult::Registered);

    ResponseCollector out;

    // 1) Rest a sell order at 100 for qty 10. A resting NewOrder that does not
    //    cross produces no Trade, but every accepted NewOrder now terminates
    //    with an Ack(Accepted) carrying its id.
    std::vector<std::uint8_t> rest;
    append_encoded(rest, NewOrder{/*order_id=*/1, Side::Sell,
                                  /*price_ticks=*/100, /*quantity=*/10,
                                  /*seq=*/0});
    REQUIRE(pipeline.submit_client_bytes(
                fd, std::span<const std::uint8_t>{rest}, out) == FrameStatus::Ok);
    REQUIRE(out.responses.size() == 1);
    {
        const auto* ack = std::get_if<Ack>(&out.responses[0].second);
        REQUIRE(ack != nullptr);
        CHECK(ack->order_id == 1);
        CHECK(ack->kind == AckKind::Accepted);
    }
    out.responses.clear();

    // 2) Send a buy at 100 for qty 4 that crosses the resting sell. The engine
    //    emits one Trade then the accepted order's terminating Ack(Accepted).
    std::vector<std::uint8_t> cross;
    append_encoded(cross, NewOrder{/*order_id=*/2, Side::Buy,
                                   /*price_ticks=*/100, /*quantity=*/4,
                                   /*seq=*/1});
    REQUIRE(pipeline.submit_client_bytes(
                fd, std::span<const std::uint8_t>{cross}, out) == FrameStatus::Ok);

    REQUIRE(out.responses.size() == 2);
    CHECK(out.responses[0].first == fd);
    const auto* trade = std::get_if<Trade>(&out.responses[0].second);
    REQUIRE(trade != nullptr);
    CHECK(trade->price_ticks == 100);  // executes at the resting limit price
    CHECK(trade->quantity == 4);       // min(incoming 4, resting 10)
    CHECK(trade->incoming_id == 2);
    CHECK(trade->resting_id == 1);
    const auto* cross_ack = std::get_if<Ack>(&out.responses[1].second);
    REQUIRE(cross_ack != nullptr);
    CHECK(cross_ack->order_id == 2);
    CHECK(cross_ack->kind == AckKind::Accepted);
    CHECK(pipeline.responses_emitted() == 3);  // Accepted(1), Trade, Accepted(2)
    CHECK(pipeline.ingress_backpressure_count() == 0);
}

TEST_CASE("wiring: a command split across two socket reads is reassembled",
          "[integration][wiring][framing]") {
    // The framing seam: a single Binary_Message split across two reads is
    // buffered and reassembled before reaching the engine, then matched and
    // answered exactly as if it had arrived whole.
    NoopPoller poller;
    TestPipeline pipeline(poller);
    REQUIRE(pipeline.initialize());

    const SocketHandle fd = 11;
    REQUIRE(pipeline.accept(fd) == AcceptResult::Registered);

    ResponseCollector out;

    // Rest a buy at 250 for qty 5. The accepted order terminates with an
    // Ack(Accepted); drain it before the split-frame portion of the test.
    std::vector<std::uint8_t> rest;
    append_encoded(rest, NewOrder{1, Side::Buy, 250, 5, 0});
    REQUIRE(pipeline.submit_client_bytes(
                fd, std::span<const std::uint8_t>{rest}, out) == FrameStatus::Ok);
    REQUIRE(out.responses.size() == 1);
    {
        const auto* ack = std::get_if<Ack>(&out.responses[0].second);
        REQUIRE(ack != nullptr);
        CHECK(ack->order_id == 1);
        CHECK(ack->kind == AckKind::Accepted);
    }
    out.responses.clear();

    // Encode a crossing sell, then feed it in two chunks that bisect the frame.
    std::vector<std::uint8_t> sell;
    append_encoded(sell, NewOrder{2, Side::Sell, 250, 5, 1});
    const std::size_t split = sell.size() / 2;

    // First half: no complete frame yet, nothing processed, connection stays
    // open with a partial frame retained.
    REQUIRE(pipeline.submit_client_bytes(
                fd, std::span<const std::uint8_t>{sell.data(), split}, out) ==
            FrameStatus::Ok);
    CHECK(out.responses.empty());

    // Second half completes the frame: the engine matches (one Trade) and then
    // emits the accepted order's terminating Ack(Accepted).
    REQUIRE(pipeline.submit_client_bytes(
                fd,
                std::span<const std::uint8_t>{sell.data() + split,
                                              sell.size() - split},
                out) == FrameStatus::Ok);

    REQUIRE(out.responses.size() == 2);
    const auto* trade = std::get_if<Trade>(&out.responses[0].second);
    REQUIRE(trade != nullptr);
    CHECK(trade->quantity == 5);
    CHECK(trade->resting_id == 1);
    CHECK(trade->incoming_id == 2);
    const auto* ack = std::get_if<Ack>(&out.responses[1].second);
    REQUIRE(ack != nullptr);
    CHECK(ack->order_id == 2);
    CHECK(ack->kind == AckKind::Accepted);
}

TEST_CASE("wiring: a cancel of a resting order returns a Cancelled ack",
          "[integration][wiring]") {
    // The cancellation path through the assembly: a CancelOrder for a resting
    // order is framed, processed, and answered with an Ack(Cancelled) that the
    // pipeline encodes back out.
    NoopPoller poller;
    TestPipeline pipeline(poller);
    REQUIRE(pipeline.initialize());

    const SocketHandle fd = 21;
    REQUIRE(pipeline.accept(fd) == AcceptResult::Registered);

    ResponseCollector out;

    // Rest a buy, then cancel it. Both commands ride in a single read to also
    // exercise multiple frames per chunk.
    std::vector<std::uint8_t> stream;
    append_encoded(stream, NewOrder{99, Side::Buy, 500, 3, 0});
    append_encoded(stream, CancelOrder{99});

    REQUIRE(pipeline.submit_client_bytes(
                fd, std::span<const std::uint8_t>{stream}, out) ==
            FrameStatus::Ok);

    // The resting buy now terminates with an Ack(Accepted); the cancel then
    // emits an Ack(Cancelled). Both ride a single read.
    REQUIRE(out.responses.size() == 2);
    const auto* accepted = std::get_if<Ack>(&out.responses[0].second);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->order_id == 99);
    CHECK(accepted->kind == AckKind::Accepted);
    const auto* ack = std::get_if<Ack>(&out.responses[1].second);
    REQUIRE(ack != nullptr);
    CHECK(ack->order_id == 99);
    CHECK(ack->kind == AckKind::Cancelled);
}

TEST_CASE("wiring: a non-operational pipeline processes nothing",
          "[integration][wiring][startup]") {
    // When engine startup reservation fails the pipeline is non-operational and
    // the data path is a no-op -- no command is processed and no response is
    // produced.
    NoopPoller poller;
    TestPipeline pipeline(poller);

    typename TestPipeline::EngineConfig cfg;
    cfg.force_reservation_failure = true;
    REQUIRE_FALSE(pipeline.initialize(cfg));
    REQUIRE_FALSE(pipeline.operational());

    const SocketHandle fd = 31;
    REQUIRE(pipeline.accept(fd) == AcceptResult::Registered);

    ResponseCollector out;
    std::vector<std::uint8_t> rest;
    append_encoded(rest, NewOrder{1, Side::Buy, 100, 1, 0});

    CHECK(pipeline.submit_client_bytes(
              fd, std::span<const std::uint8_t>{rest}, out) ==
          FrameStatus::Closed);
    CHECK(out.responses.empty());
    CHECK(pipeline.responses_emitted() == 0);
}
