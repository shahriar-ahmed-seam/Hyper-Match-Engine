// End-to-end integration test of the full round trip.
//
//     Client -> Gateway -> Engine -> Gateway -> Client
//
// Why this is a C++ pipeline test that MODELS the Gateway edges
// ------------------------------------------------------------
// The Gateway is Rust (gateway/src/lib.rs) and the Network_Server / Matching_
// Engine are C++ (cpp/server/.../server_pipeline.hpp). A single in-process
// cross-language test is impractical, so the round trip is exercised here as a
// faithful C++ test that drives the real ServerPipeline (framing -> ingress ring
// -> Matching_Engine -> egress ring -> response bytes) and models the Gateway's
// two client-edge translations with the C++ Binary_Codec:
//
//   * Inbound  (client -> Gateway): a JSON-equivalent order is turned into a
//     NewOrder Binary_Message and ENCODED to wire bytes -- exactly what the Rust
//     Gateway produces.
//   * Outbound (Gateway -> client): the response wire bytes the pipeline emits
//     are DECODED back into a Binary_Message -- exactly what the Rust Gateway
//     consumes.
//
// This is faithful because the Rust Gateway and this C++ codec agree on the
// Wire_Protocol byte-for-byte, pinned by the cross-implementation test. The
// bytes the Gateway would put on the ingress ring for an accepted order are
// byte-identical to the bytes encoded here, and the response bytes this pipeline
// emits are byte-identical to the bytes the Gateway would decode. The Rust half
// of the same round trip (JSON<->bytes translation) is asserted directly in
// gateway/tests/end_to_end.rs; together the two cover the whole client ->
// Gateway -> Engine -> Gateway -> client path across the shared Wire_Protocol
// contract.
//
// Scope: this is the end-to-end ROUND-TRIP test. Per-component correctness
// (matching, framing, codec, lifecycle) and the data-path wiring smoke test
// live in their own files; the Gateway response-conversion latency bound is a
// separate sibling.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
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

namespace {

// A no-op readiness poller: the round-trip test does not model OS socket
// resources (covered by the lifecycle integration tests), only satisfies the
// ConnectionRegistry the pipeline owns.
struct NoopPoller : ReadinessPoller {
    void register_readiness(SocketHandle) override {}
    void deregister_readiness(SocketHandle) override {}
    void close_socket(SocketHandle) override {}
};

// ---- The client edge: what the Rust Gateway does at the boundary ----------

// Inbound translation modeled: a validated client order (the result of the
// Gateway's handler over a JSON body) is encoded to the NewOrder wire bytes the
// Gateway forwards to the engine. Encoding into a sized buffer and appending the
// exact written length mirrors the Gateway's encoding step.
void client_submit(std::vector<std::uint8_t>& stream, const BinaryMessage& cmd) {
    const std::size_t at = stream.size();
    stream.resize(at + encoded_len_of(cmd));
    const auto res =
        encode(cmd, std::span<std::uint8_t>{stream.data() + at, encoded_len_of(cmd)});
    REQUIRE(res.is_ok());
}

// Outbound translation modeled: the response wire bytes the pipeline emits are
// decoded back into a Binary_Message, exactly as the Gateway does before
// building the JSON response. Collected per originating connection so the round
// trip back to the right client is asserted.
struct ClientInbox {
    std::vector<std::pair<SocketHandle, BinaryMessage>> received;

    void operator()(SocketHandle fd, std::span<const std::uint8_t> frame) {
        const auto decoded = decode(frame);
        REQUIRE(decoded.is_ok());
        received.emplace_back(fd, decoded.value());
    }
};

// A modestly-sized pipeline keeps the test cheap while composing the real
// framing / ring / engine / encoding stack.
using Pipeline = server::ServerPipeline</*MaxOrders=*/256,
                                        /*MaxPriceLevels=*/64,
                                        /*IngressCapacity=*/64,
                                        /*EgressCapacity=*/64>;

}  // namespace

TEST_CASE("e2e: a crossing order round-trips a Trade back to the client",
          "[integration][e2e]") {
    // Full round trip: a client rests a sell, then a second client order
    // crosses it; the resulting Trade travels Engine -> egress -> response bytes
    // -> (Gateway decode) and arrives back at the originating client carrying
    // the execution details.
    NoopPoller poller;
    Pipeline pipeline(poller);
    REQUIRE(pipeline.initialize());
    REQUIRE(pipeline.operational());

    const SocketHandle client = 7;
    REQUIRE(pipeline.accept(client) == AcceptResult::Registered);

    ClientInbox inbox;

    // Client submits a resting sell: { side: "sell", price: 100, qty: 10 }.
    // A non-crossing order produces no Trade, but every accepted NewOrder now
    // round-trips a terminating Ack(Accepted) carrying its id.
    std::vector<std::uint8_t> rest_bytes;
    client_submit(rest_bytes,
                  NewOrder{/*order_id=*/1, Side::Sell,
                           /*price_ticks=*/100, /*quantity=*/10, /*seq=*/0});
    REQUIRE(pipeline.submit_client_bytes(
                client, std::span<const std::uint8_t>{rest_bytes}, inbox) ==
            FrameStatus::Ok);
    REQUIRE(inbox.received.size() == 1);
    {
        const auto* ack = std::get_if<Ack>(&inbox.received[0].second);
        REQUIRE(ack != nullptr);
        CHECK(ack->order_id == 1);
        CHECK(ack->kind == AckKind::Accepted);
    }
    inbox.received.clear();

    // Client submits a crossing buy: { side: "buy", price: 100, qty: 4 }.
    std::vector<std::uint8_t> cross_bytes;
    client_submit(cross_bytes,
                  NewOrder{/*order_id=*/2, Side::Buy,
                           /*price_ticks=*/100, /*quantity=*/4, /*seq=*/1});
    REQUIRE(pipeline.submit_client_bytes(
                client, std::span<const std::uint8_t>{cross_bytes}, inbox) ==
            FrameStatus::Ok);

    // The Trade has round-tripped all the way back to the originating client,
    // followed by the accepted order's terminating Ack(Accepted).
    REQUIRE(inbox.received.size() == 2);
    CHECK(inbox.received[0].first == client);
    const auto* trade = std::get_if<Trade>(&inbox.received[0].second);
    REQUIRE(trade != nullptr);
    CHECK(trade->price_ticks == 100);  // executes at the resting limit price
    CHECK(trade->quantity == 4);       // min(incoming 4, resting 10)
    CHECK(trade->incoming_id == 2);
    CHECK(trade->resting_id == 1);
    const auto* cross_ack = std::get_if<Ack>(&inbox.received[1].second);
    REQUIRE(cross_ack != nullptr);
    CHECK(cross_ack->order_id == 2);
    CHECK(cross_ack->kind == AckKind::Accepted);
    CHECK(pipeline.responses_emitted() == 3);  // Accepted(1), Trade, Accepted(2)
    CHECK(pipeline.ingress_backpressure_count() == 0);
}

TEST_CASE("e2e: submit then cancel round-trips a Cancelled Ack to the client",
          "[integration][e2e]") {
    // Full round trip for the cancellation path: a client rests a buy and then
    // cancels it; the engine's cancellation acknowledgement travels back through
    // the pipeline and arrives at the client as an Ack(Cancelled) for the same
    // order id.
    NoopPoller poller;
    Pipeline pipeline(poller);
    REQUIRE(pipeline.initialize());
    REQUIRE(pipeline.operational());

    const SocketHandle client = 21;
    REQUIRE(pipeline.accept(client) == AcceptResult::Registered);

    ClientInbox inbox;

    // Rest a buy, then cancel it. Both client commands ride a single read,
    // exercising multiple framed commands per inbound chunk.
    std::vector<std::uint8_t> stream;
    client_submit(stream, NewOrder{/*order_id=*/99, Side::Buy,
                                   /*price_ticks=*/500, /*quantity=*/3,
                                   /*seq=*/0});
    client_submit(stream, CancelOrder{/*order_id=*/99});

    REQUIRE(pipeline.submit_client_bytes(
                client, std::span<const std::uint8_t>{stream}, inbox) ==
            FrameStatus::Ok);

    // The resting buy returns its terminating Ack(Accepted); the cancel then
    // returns an Ack(Cancelled) for the same order id.
    REQUIRE(inbox.received.size() == 2);
    CHECK(inbox.received[0].first == client);
    const auto* accepted = std::get_if<Ack>(&inbox.received[0].second);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->order_id == 99);
    CHECK(accepted->kind == AckKind::Accepted);
    CHECK(inbox.received[1].first == client);
    const auto* ack = std::get_if<Ack>(&inbox.received[1].second);
    REQUIRE(ack != nullptr);
    CHECK(ack->order_id == 99);
    CHECK(ack->kind == AckKind::Cancelled);
    CHECK(pipeline.responses_emitted() == 2);
}
