// Codec encode/decode round trip. For any valid Binary_Message, encoding it and
// then decoding the resulting byte sequence yields a Binary_Message equal to
// the original, with no loss of any field value.
//
// The RapidCheck Arbitrary generators below produce every Binary_Message
// variant with in-range price_ticks and quantity (the only fields the codec
// range-checks), so every generated message is a valid input to encode().

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

// ---------------------------------------------------------------------------
// RapidCheck Arbitrary generators for the message variants.
//
// Enum fields are drawn from their full discriminator set; the range-checked
// numeric fields (price_ticks, quantity) are constrained to the Wire_Protocol
// permitted ranges so encode() always succeeds. All other integer fields span
// the full u64/u32 domain via gen::arbitrary, exercising boundary values.
// ---------------------------------------------------------------------------

namespace rc {

template <>
struct Arbitrary<Side> {
    static Gen<Side> arbitrary() {
        return gen::element(Side::Buy, Side::Sell);
    }
};

template <>
struct Arbitrary<AckKind> {
    static Gen<AckKind> arbitrary() {
        return gen::element(AckKind::Accepted, AckKind::Cancelled);
    }
};

template <>
struct Arbitrary<RejectReason> {
    static Gen<RejectReason> arbitrary() {
        return gen::element(RejectReason::InvalidPrice,
                            RejectReason::InvalidQuantity,
                            RejectReason::OrderNotFound,
                            RejectReason::NoLongerResting,
                            RejectReason::IntegrityViolation);
    }
};

}  // namespace rc

namespace {

// In-range price_ticks: [kMinPriceTicks, kMaxPriceTicks]. gen::inRange is
// half-open, so the upper bound is +1.
rc::Gen<std::uint64_t> gen_price_ticks() {
    return rc::gen::inRange<std::uint64_t>(limits::kMinPriceTicks,
                                           limits::kMaxPriceTicks + 1);
}

// In-range quantity: [kMinGatewayQuantity, kMaxGatewayQuantity].
rc::Gen<std::uint32_t> gen_quantity() {
    return rc::gen::inRange<std::uint32_t>(limits::kMinGatewayQuantity,
                                           limits::kMaxGatewayQuantity + 1);
}

rc::Gen<BinaryMessage> gen_new_order() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<Side>(), gen_price_ticks(),
                       gen_quantity(), rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, Side, std::uint64_t, std::uint32_t,
                            std::uint64_t>& t) -> BinaryMessage {
            return NewOrder{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                            std::get<3>(t), std::get<4>(t)};
        });
}

rc::Gen<BinaryMessage> gen_cancel_order() {
    return rc::gen::map(
        rc::gen::arbitrary<std::uint64_t>(),
        [](std::uint64_t order_id) -> BinaryMessage {
            return CancelOrder{order_id};
        });
}

rc::Gen<BinaryMessage> gen_trade() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_price_ticks(),
                       gen_quantity(), rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                            std::uint64_t, std::uint64_t>& t) -> BinaryMessage {
            return Trade{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                         std::get<3>(t), std::get<4>(t)};
        });
}

rc::Gen<BinaryMessage> gen_ack() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<AckKind>()),
        [](const std::tuple<std::uint64_t, AckKind>& t) -> BinaryMessage {
            return Ack{std::get<0>(t), std::get<1>(t)};
        });
}

rc::Gen<BinaryMessage> gen_reject() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<RejectReason>()),
        [](const std::tuple<std::uint64_t, RejectReason>& t) -> BinaryMessage {
            return Reject{std::get<0>(t), std::get<1>(t)};
        });
}

}  // namespace

namespace rc {

// A valid Binary_Message of any of the five variants, uniformly chosen.
template <>
struct Arbitrary<BinaryMessage> {
    static Gen<BinaryMessage> arbitrary() {
        return gen::oneOf(gen_new_order(), gen_cancel_order(), gen_trade(),
                          gen_ack(), gen_reject());
    }
};

}  // namespace rc

TEST_CASE("Property 4: codec encode/decode round trip preserves the message",
          "[codec][property][roundtrip]") {
    const bool ok = rc::check(
        "for any valid Binary_Message, decode(encode(msg)) == msg",
        [](const BinaryMessage& msg) {
            // Encode into an exactly-sized buffer; a valid message always
            // encodes successfully.
            std::vector<std::uint8_t> buf(encoded_len_of(msg), 0);
            auto encoded = encode(msg, std::span<std::uint8_t>{buf});
            RC_ASSERT(encoded.is_ok());
            RC_ASSERT(encoded.value() == encoded_len_of(msg));

            // Decoding the encoded bytes reproduces the original message with
            // no field loss.
            auto decoded = decode(std::span<const std::uint8_t>{buf});
            RC_ASSERT(decoded.is_ok());
            RC_ASSERT(decoded.value() == msg);
        });
    CHECK(ok);
}
