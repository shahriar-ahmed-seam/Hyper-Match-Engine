// Scaffolding tests for the shared Wire_Protocol primitives.
//
// These confirm two things:
//  1. The test harness is integrated - Catch2 is the unit-test runner and
//     RapidCheck is available for property-based testing. The RapidCheck check
//     below is an integration sanity check, not one of the numbered correctness
//     properties (those live in the dedicated property tests).
//  2. The shared Side / AckKind / RejectReason enums and the Wire_Protocol
//     layout constants are correct.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstdint>
#include <string_view>

#include "hme/engine.hpp"
#include "hme/network.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;

// RapidCheck generator for the Side enum, used by the integration check below.
// Must be visible before the rc::check call that relies on it.
namespace rc {
template <>
struct Arbitrary<Side> {
    static Gen<Side> arbitrary() {
        return gen::element(Side::Buy, Side::Sell);
    }
};
}  // namespace rc

TEST_CASE("Side maps to and from its Wire_Protocol byte", "[wire][side]") {
    CHECK(side_as_byte(Side::Buy) == 0);
    CHECK(side_as_byte(Side::Sell) == 1);
    CHECK(side_from_byte(0) == Side::Buy);
    CHECK(side_from_byte(1) == Side::Sell);
    CHECK(side_from_byte(2) == std::nullopt);
}

TEST_CASE("AckKind maps to and from its Wire_Protocol byte", "[wire][ack]") {
    CHECK(ack_kind_as_byte(AckKind::Accepted) == 0);
    CHECK(ack_kind_as_byte(AckKind::Cancelled) == 1);
    CHECK(ack_kind_from_byte(0) == AckKind::Accepted);
    CHECK(ack_kind_from_byte(1) == AckKind::Cancelled);
    CHECK(ack_kind_from_byte(7) == std::nullopt);
}

TEST_CASE("RejectReason maps to and from its Wire_Protocol byte", "[wire][reject]") {
    CHECK(reject_reason_as_byte(RejectReason::InvalidPrice) == 0);
    CHECK(reject_reason_as_byte(RejectReason::InvalidQuantity) == 1);
    CHECK(reject_reason_as_byte(RejectReason::OrderNotFound) == 2);
    CHECK(reject_reason_as_byte(RejectReason::NoLongerResting) == 3);
    CHECK(reject_reason_as_byte(RejectReason::IntegrityViolation) == 4);
    CHECK(reject_reason_from_byte(4) == RejectReason::IntegrityViolation);
    CHECK(reject_reason_from_byte(99) == std::nullopt);
}

TEST_CASE("Wire_Protocol message lengths match the design layout", "[wire][layout]") {
    CHECK(wire::kNewOrderLen == 30);
    CHECK(wire::kCancelOrderLen == 9);
    CHECK(wire::kTradeLen == 37);
    CHECK(wire::kAckLen == 10);
    CHECK(wire::kRejectLen == 10);
    CHECK(wire::kMaxMessageLen == wire::kTradeLen);

    CHECK(wire::message_len(wire::MessageType::NewOrder) == wire::kNewOrderLen);
    CHECK(wire::message_len(wire::MessageType::CancelOrder) == wire::kCancelOrderLen);
    CHECK(wire::message_len(wire::MessageType::Trade) == wire::kTradeLen);
    CHECK(wire::message_len(wire::MessageType::Ack) == wire::kAckLen);
    CHECK(wire::message_len(wire::MessageType::Reject) == wire::kRejectLen);
}

TEST_CASE("Message-type discriminator bytes round trip", "[wire][type]") {
    using wire::MessageType;
    for (auto t : {MessageType::NewOrder, MessageType::CancelOrder,
                   MessageType::Trade, MessageType::Ack, MessageType::Reject}) {
        CHECK(wire::message_type_from_byte(wire::message_type_as_byte(t)) == t);
    }
    CHECK(wire::message_type_from_byte(200) == std::nullopt);
}

TEST_CASE("Component targets link and are reachable", "[scaffold]") {
    CHECK(std::string_view{engine::component_name()} == "Matching_Engine");
    CHECK(std::string_view{network::component_name()} == "Network_Server");
    CHECK(network::kMaxConnections == 10000);
    CHECK(network::kMaxMessageSize == 65536);
}

TEST_CASE("RapidCheck is integrated and runs", "[scaffold][rapidcheck]") {
    // Integration sanity check only: every Side discriminator byte round-trips.
    const bool ok = rc::check(
        "Side discriminator byte round trips",
        [](Side side) {
            RC_ASSERT(side_from_byte(side_as_byte(side)) == side);
        });
    CHECK(ok);
}
