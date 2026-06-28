// Codec decode/encode byte round trip. For any byte sequence the Binary_Codec
// decodes successfully, decoding it and then re-encoding the resulting
// Binary_Message yields a byte sequence equal to the original bytes.
//
// The set of "byte sequences the codec decodes successfully" is exactly the
// image of encode() over valid Binary_Messages: a fixed-layout, fixed-length,
// type-discriminated encoding with no redundant or free bits, so every
// decodable byte sequence is the encoding of exactly one valid message. We
// therefore generate a valid message, encode it to obtain a known-decodable
// byte sequence `original_bytes`, and assert that encode(decode(original_bytes))
// reproduces `original_bytes` byte-for-byte.
//
// The RapidCheck generators below are defined locally in an anonymous namespace
// (internal linkage) and use distinct names. This deliberately avoids the
// rc::Arbitrary<...> specializations declared in prop_codec_round_trip_test.cpp:
// re-declaring those specializations in this translation unit would be an ODR /
// duplicate-symbol violation when both tests link into the same hme_tests
// executable.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;

namespace {

// ---------------------------------------------------------------------------
// Local generators (internal linkage; distinct names from the round-trip
// property test's file).
//
// Enum fields are drawn from their full discriminator set; the range-checked
// numeric fields (price_ticks, quantity) are constrained to the Wire_Protocol
// permitted ranges so encode() always succeeds. All other integer fields span
// the full u64/u32 domain, exercising boundary values.
// ---------------------------------------------------------------------------

rc::Gen<Side> gen_side_p5() {
    return rc::gen::element(Side::Buy, Side::Sell);
}

rc::Gen<AckKind> gen_ack_kind_p5() {
    return rc::gen::element(AckKind::Accepted, AckKind::Cancelled);
}

rc::Gen<RejectReason> gen_reject_reason_p5() {
    return rc::gen::element(RejectReason::InvalidPrice,
                            RejectReason::InvalidQuantity,
                            RejectReason::OrderNotFound,
                            RejectReason::NoLongerResting,
                            RejectReason::IntegrityViolation);
}

// In-range price_ticks: [kMinPriceTicks, kMaxPriceTicks]. gen::inRange is
// half-open, so the upper bound is +1.
rc::Gen<std::uint64_t> gen_price_ticks_p5() {
    return rc::gen::inRange<std::uint64_t>(limits::kMinPriceTicks,
                                           limits::kMaxPriceTicks + 1);
}

// In-range quantity: [kMinGatewayQuantity, kMaxGatewayQuantity].
rc::Gen<std::uint32_t> gen_quantity_p5() {
    return rc::gen::inRange<std::uint32_t>(limits::kMinGatewayQuantity,
                                           limits::kMaxGatewayQuantity + 1);
}

rc::Gen<BinaryMessage> gen_new_order_p5() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_side_p5(),
                       gen_price_ticks_p5(), gen_quantity_p5(),
                       rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, Side, std::uint64_t, std::uint32_t,
                            std::uint64_t>& t) -> BinaryMessage {
            return NewOrder{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                            std::get<3>(t), std::get<4>(t)};
        });
}

rc::Gen<BinaryMessage> gen_cancel_order_p5() {
    return rc::gen::map(
        rc::gen::arbitrary<std::uint64_t>(),
        [](std::uint64_t order_id) -> BinaryMessage {
            return CancelOrder{order_id};
        });
}

rc::Gen<BinaryMessage> gen_trade_p5() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_price_ticks_p5(),
                       gen_quantity_p5(), rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                            std::uint64_t, std::uint64_t>& t) -> BinaryMessage {
            return Trade{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                         std::get<3>(t), std::get<4>(t)};
        });
}

rc::Gen<BinaryMessage> gen_ack_p5() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_ack_kind_p5()),
        [](const std::tuple<std::uint64_t, AckKind>& t) -> BinaryMessage {
            return Ack{std::get<0>(t), std::get<1>(t)};
        });
}

rc::Gen<BinaryMessage> gen_reject_p5() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(),
                       gen_reject_reason_p5()),
        [](const std::tuple<std::uint64_t, RejectReason>& t) -> BinaryMessage {
            return Reject{std::get<0>(t), std::get<1>(t)};
        });
}

// A valid Binary_Message of any of the five variants, uniformly chosen.
rc::Gen<BinaryMessage> gen_message_p5() {
    return rc::gen::oneOf(gen_new_order_p5(), gen_cancel_order_p5(),
                          gen_trade_p5(), gen_ack_p5(), gen_reject_p5());
}

}  // namespace

TEST_CASE("Property 5: codec decode/encode byte round trip preserves the bytes",
          "[codec][property][roundtrip]") {
    const bool ok = rc::check(
        "for any decodable byte sequence, encode(decode(bytes)) == bytes",
        [] {
            // Produce a known-decodable byte sequence by encoding a valid
            // message. Because the Wire_Protocol is fixed-layout and
            // fixed-length with no free bits, this spans exactly the set of
            // byte sequences decode() accepts.
            const BinaryMessage msg = *gen_message_p5();
            std::vector<std::uint8_t> original_bytes(encoded_len_of(msg), 0);
            auto first = encode(msg, std::span<std::uint8_t>{original_bytes});
            RC_ASSERT(first.is_ok());
            RC_ASSERT(first.value() == original_bytes.size());

            // Decoding the bytes must succeed (precondition of the property).
            auto decoded =
                decode(std::span<const std::uint8_t>{original_bytes});
            RC_ASSERT(decoded.is_ok());

            // Re-encoding the decoded message reproduces the original bytes
            // exactly.
            std::vector<std::uint8_t> reencoded_bytes(
                encoded_len_of(decoded.value()), 0);
            auto second = encode(decoded.value(),
                                 std::span<std::uint8_t>{reencoded_bytes});
            RC_ASSERT(second.is_ok());
            RC_ASSERT(second.value() == reencoded_bytes.size());

            RC_ASSERT(reencoded_bytes == original_bytes);
        });
    CHECK(ok);
}
