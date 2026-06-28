// Codec rejects malformed input. For any byte sequence that declares an unknown
// message type, is shorter than its declared type requires, or carries excess
// trailing bytes, decoding returns the corresponding error and no
// Binary_Message (and on insufficient length the codec state is preserved); and
// for any Binary_Message containing a field outside its protocol-permitted
// range, encoding returns an out-of-range error and no byte sequence.
//
// A single generator produces a tagged "malformed case" drawn uniformly from
// every malformed family the codec must reject, and the property asserts the
// matching typed CodecError is returned without a Binary_Message (and without
// crashing):
//
//   - unknown type byte                          -> UnknownType
//   - valid type but too few bytes               -> InsufficientLength
//   - valid type but excess trailing bytes       -> ExcessTrailingBytes
//   - invalid enum discriminator (side/kind/...) -> FieldOutOfRange
//   - out-of-range price_ticks / quantity bytes  -> FieldOutOfRange
//   - encode of a message with an out-of-range   -> FieldOutOfRange
//     numeric field
//
// All RapidCheck generators below are defined locally in an anonymous namespace
// (internal linkage) and use distinct `_p7` names. This deliberately avoids the
// rc::Arbitrary<...> specializations declared in prop_codec_round_trip_test.cpp:
// re-declaring those specializations in this translation unit would be an ODR /
// duplicate-symbol violation when both tests link into the same hme_tests
// executable. It likewise avoids clashing with the local helpers in
// prop_codec_byte_round_trip_test.cpp, which use `_p5` names.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;

namespace {

// ---------------------------------------------------------------------------
// A single malformed-input case carrying the operation to exercise and the
// CodecErrorKind the codec must return.
//
//   is_encode == true  : call encode(msg) and expect `expected`.
//   is_encode == false : call decode(bytes) and expect `expected`.
// ---------------------------------------------------------------------------
struct MalformedCase {
    bool is_encode = false;
    BinaryMessage msg{CancelOrder{}};   // used when is_encode
    std::vector<std::uint8_t> bytes{};  // used when !is_encode
    CodecErrorKind expected = CodecErrorKind::InsufficientLength;
};

// ----- little-endian field patchers (overwrite a field in an encoded buffer) -

void put_u32_le_p7(std::vector<std::uint8_t>& b, std::size_t off,
                   std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

void put_u64_le_p7(std::vector<std::uint8_t>& b, std::size_t off,
                   std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

// ----- in-range and out-of-range numeric field generators -------------------

// In-range price_ticks: [kMinPriceTicks, kMaxPriceTicks] (inRange is half-open).
rc::Gen<std::uint64_t> gen_in_price_p7() {
    return rc::gen::inRange<std::uint64_t>(limits::kMinPriceTicks,
                                           limits::kMaxPriceTicks + 1);
}

// In-range quantity: [kMinGatewayQuantity, kMaxGatewayQuantity].
rc::Gen<std::uint32_t> gen_in_qty_p7() {
    return rc::gen::inRange<std::uint32_t>(limits::kMinGatewayQuantity,
                                           limits::kMaxGatewayQuantity + 1);
}

// Out-of-range price_ticks: either 0 (below the minimum of 1) or strictly above
// the maximum permitted tick.
rc::Gen<std::uint64_t> gen_oor_price_p7() {
    return rc::gen::oneOf(
        rc::gen::just(std::uint64_t{0}),
        rc::gen::inRange<std::uint64_t>(
            limits::kMaxPriceTicks + 1,
            std::numeric_limits<std::uint64_t>::max()));
}

// Out-of-range quantity: either 0 (below the minimum of 1) or strictly above
// the maximum permitted quantity.
rc::Gen<std::uint32_t> gen_oor_qty_p7() {
    return rc::gen::oneOf(
        rc::gen::just(std::uint32_t{0}),
        rc::gen::inRange<std::uint32_t>(
            limits::kMaxGatewayQuantity + 1,
            std::numeric_limits<std::uint32_t>::max()));
}

// ---------------------------------------------------------------------------
// Family 1: unknown message-type discriminator byte.
//
// The leading byte names no known type (>= 5), followed by an arbitrary tail.
// decode() checks the type byte before length, so any tail length is rejected
// as UnknownType.
// ---------------------------------------------------------------------------
rc::Gen<MalformedCase> gen_unknown_type_p7() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange<int>(5, 256),
                       rc::gen::arbitrary<std::vector<std::uint8_t>>()),
        [](const std::tuple<int, std::vector<std::uint8_t>>& t) {
            MalformedCase c;
            c.is_encode = false;
            c.bytes.push_back(static_cast<std::uint8_t>(std::get<0>(t)));
            const auto& tail = std::get<1>(t);
            c.bytes.insert(c.bytes.end(), tail.begin(), tail.end());
            c.expected = CodecErrorKind::UnknownType;
            return c;
        });
}

// ---------------------------------------------------------------------------
// Family 2: valid type byte but fewer bytes than the type requires.
//
// Length is drawn from [1, needed - 1] so the buffer is non-empty (the caller
// state is preserved) yet too short. decode() validates the exact length before
// reading any field, so the field content is irrelevant.
// ---------------------------------------------------------------------------
rc::Gen<MalformedCase> gen_too_short_p7() {
    return rc::gen::mapcat(
        rc::gen::inRange<int>(0, 5), [](int type_idx) {
            const auto type = static_cast<wire::MessageType>(type_idx);
            const std::size_t needed = *wire::message_len(type);
            return rc::gen::map(
                rc::gen::inRange<std::size_t>(1, needed),
                [type_idx](std::size_t len) {
                    MalformedCase c;
                    c.is_encode = false;
                    c.bytes.assign(len, 0);
                    c.bytes[0] = static_cast<std::uint8_t>(type_idx);
                    c.expected = CodecErrorKind::InsufficientLength;
                    return c;
                });
        });
}

// ---------------------------------------------------------------------------
// Family 3: valid type byte but more bytes than the type requires.
//
// Length is needed + extra (extra >= 1). decode() rejects the excess length
// before reading any field.
// ---------------------------------------------------------------------------
rc::Gen<MalformedCase> gen_too_long_p7() {
    return rc::gen::mapcat(
        rc::gen::inRange<int>(0, 5), [](int type_idx) {
            const auto type = static_cast<wire::MessageType>(type_idx);
            const std::size_t needed = *wire::message_len(type);
            return rc::gen::map(
                rc::gen::inRange<std::size_t>(1, 64),
                [type_idx, needed](std::size_t extra) {
                    MalformedCase c;
                    c.is_encode = false;
                    c.bytes.assign(needed + extra, 0);
                    c.bytes[0] = static_cast<std::uint8_t>(type_idx);
                    c.expected = CodecErrorKind::ExcessTrailingBytes;
                    return c;
                });
        });
}

// ---------------------------------------------------------------------------
// Family 4: correctly-sized buffer with an invalid enum discriminator.
//
// Each base message is built from in-range values and encoded, then the enum
// discriminator byte at offset 9 is overwritten with an out-of-range value.
// decode() validates the discriminator and reports FieldOutOfRange.
// ---------------------------------------------------------------------------
rc::Gen<MalformedCase> gen_invalid_discriminator_p7() {
    // NewOrder.side at offset 9; valid values {0,1}, invalid >= 2.
    auto new_order = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_in_price_p7(),
                       gen_in_qty_p7(), rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::inRange<int>(2, 256)),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                            std::uint64_t, int>& t) {
            const NewOrder n{std::get<0>(t), Side::Buy, std::get<1>(t),
                             std::get<2>(t), std::get<3>(t)};
            std::vector<std::uint8_t> b(wire::kNewOrderLen, 0);
            (void)encode(BinaryMessage{n}, std::span<std::uint8_t>{b});
            b[9] = static_cast<std::uint8_t>(std::get<4>(t));
            MalformedCase c;
            c.is_encode = false;
            c.bytes = std::move(b);
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    // Ack.kind at offset 9; valid values {0,1}, invalid >= 2.
    auto ack = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::inRange<int>(2, 256)),
        [](const std::tuple<std::uint64_t, int>& t) {
            const Ack a{std::get<0>(t), AckKind::Accepted};
            std::vector<std::uint8_t> b(wire::kAckLen, 0);
            (void)encode(BinaryMessage{a}, std::span<std::uint8_t>{b});
            b[9] = static_cast<std::uint8_t>(std::get<1>(t));
            MalformedCase c;
            c.is_encode = false;
            c.bytes = std::move(b);
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    // Reject.reason at offset 9; valid values {0..4}, invalid >= 5.
    auto reject = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::inRange<int>(5, 256)),
        [](const std::tuple<std::uint64_t, int>& t) {
            const Reject r{std::get<0>(t), RejectReason::InvalidPrice};
            std::vector<std::uint8_t> b(wire::kRejectLen, 0);
            (void)encode(BinaryMessage{r}, std::span<std::uint8_t>{b});
            b[9] = static_cast<std::uint8_t>(std::get<1>(t));
            MalformedCase c;
            c.is_encode = false;
            c.bytes = std::move(b);
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    return rc::gen::oneOf(std::move(new_order), std::move(ack),
                          std::move(reject));
}

// ---------------------------------------------------------------------------
// Family 5: correctly-sized buffer whose numeric field is out of range. A valid
// base message is encoded, then its price_ticks or quantity field is
// overwritten with an out-of-range value. decode() reports FieldOutOfRange.
//
// NewOrder: price_ticks @ 10 (u64), quantity @ 18 (u32).
// Trade:    price_ticks @  9 (u64), quantity @ 17 (u32).
// ---------------------------------------------------------------------------
rc::Gen<MalformedCase> gen_oor_numeric_decode_p7() {
    auto new_order_price = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_in_qty_p7(),
                       rc::gen::arbitrary<std::uint64_t>(), gen_oor_price_p7()),
        [](const std::tuple<std::uint64_t, std::uint32_t, std::uint64_t,
                            std::uint64_t>& t) {
            const NewOrder n{std::get<0>(t), Side::Buy, limits::kMinPriceTicks,
                             std::get<1>(t), std::get<2>(t)};
            std::vector<std::uint8_t> b(wire::kNewOrderLen, 0);
            (void)encode(BinaryMessage{n}, std::span<std::uint8_t>{b});
            put_u64_le_p7(b, 10, std::get<3>(t));  // out-of-range price_ticks
            MalformedCase c;
            c.is_encode = false;
            c.bytes = std::move(b);
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    auto new_order_qty = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_in_price_p7(),
                       rc::gen::arbitrary<std::uint64_t>(), gen_oor_qty_p7()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                            std::uint32_t>& t) {
            const NewOrder n{std::get<0>(t), Side::Buy, std::get<1>(t),
                             limits::kMinGatewayQuantity, std::get<2>(t)};
            std::vector<std::uint8_t> b(wire::kNewOrderLen, 0);
            (void)encode(BinaryMessage{n}, std::span<std::uint8_t>{b});
            put_u32_le_p7(b, 18, std::get<3>(t));  // out-of-range quantity
            MalformedCase c;
            c.is_encode = false;
            c.bytes = std::move(b);
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    auto trade_price = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_in_qty_p7(),
                       rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<std::uint64_t>(), gen_oor_price_p7()),
        [](const std::tuple<std::uint64_t, std::uint32_t, std::uint64_t,
                            std::uint64_t, std::uint64_t>& t) {
            const Trade tr{std::get<0>(t), limits::kMinPriceTicks,
                           std::get<1>(t), std::get<2>(t), std::get<3>(t)};
            std::vector<std::uint8_t> b(wire::kTradeLen, 0);
            (void)encode(BinaryMessage{tr}, std::span<std::uint8_t>{b});
            put_u64_le_p7(b, 9, std::get<4>(t));  // out-of-range price_ticks
            MalformedCase c;
            c.is_encode = false;
            c.bytes = std::move(b);
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    auto trade_qty = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_in_price_p7(),
                       rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<std::uint64_t>(), gen_oor_qty_p7()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                            std::uint64_t, std::uint32_t>& t) {
            const Trade tr{std::get<0>(t), std::get<1>(t),
                           limits::kMinGatewayQuantity, std::get<2>(t),
                           std::get<3>(t)};
            std::vector<std::uint8_t> b(wire::kTradeLen, 0);
            (void)encode(BinaryMessage{tr}, std::span<std::uint8_t>{b});
            put_u32_le_p7(b, 17, std::get<4>(t));  // out-of-range quantity
            MalformedCase c;
            c.is_encode = false;
            c.bytes = std::move(b);
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    return rc::gen::oneOf(std::move(new_order_price), std::move(new_order_qty),
                          std::move(trade_price), std::move(trade_qty));
}

// ---------------------------------------------------------------------------
// Family 6: encoding a Binary_Message whose numeric field is out of range.
// encode() must return FieldOutOfRange and write no byte sequence. Only
// NewOrder and Trade carry range-checked numeric fields.
// ---------------------------------------------------------------------------
rc::Gen<MalformedCase> gen_oor_encode_p7() {
    auto new_order_price = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_oor_price_p7(),
                       gen_in_qty_p7(), rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                            std::uint64_t>& t) {
            MalformedCase c;
            c.is_encode = true;
            c.msg = NewOrder{std::get<0>(t), Side::Buy, std::get<1>(t),
                             std::get<2>(t), std::get<3>(t)};
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    auto new_order_qty = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_in_price_p7(),
                       gen_oor_qty_p7(), rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                            std::uint64_t>& t) {
            MalformedCase c;
            c.is_encode = true;
            c.msg = NewOrder{std::get<0>(t), Side::Buy, std::get<1>(t),
                             std::get<2>(t), std::get<3>(t)};
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    auto trade_price = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_oor_price_p7(),
                       gen_in_qty_p7(), rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                            std::uint64_t, std::uint64_t>& t) {
            MalformedCase c;
            c.is_encode = true;
            c.msg = Trade{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                          std::get<3>(t), std::get<4>(t)};
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    auto trade_qty = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_in_price_p7(),
                       gen_oor_qty_p7(), rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                            std::uint64_t, std::uint64_t>& t) {
            MalformedCase c;
            c.is_encode = true;
            c.msg = Trade{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                          std::get<3>(t), std::get<4>(t)};
            c.expected = CodecErrorKind::FieldOutOfRange;
            return c;
        });

    return rc::gen::oneOf(std::move(new_order_price), std::move(new_order_qty),
                          std::move(trade_price), std::move(trade_qty));
}

// A malformed case drawn uniformly from every family above.
rc::Gen<MalformedCase> gen_malformed_case_p7() {
    return rc::gen::oneOf(gen_unknown_type_p7(), gen_too_short_p7(),
                          gen_too_long_p7(), gen_invalid_discriminator_p7(),
                          gen_oor_numeric_decode_p7(), gen_oor_encode_p7());
}

}  // namespace

TEST_CASE("Property 7: codec rejects malformed input with the appropriate error",
          "[codec][property][malformed]") {
    const bool ok = rc::check(
        "malformed input yields the matching typed error and no message",
        [] {
            const MalformedCase c = *gen_malformed_case_p7();

            if (c.is_encode) {
                // Encode of an out-of-range message: a buffer large enough for
                // any message ensures the only possible failure is the
                // out-of-range field, not insufficient room.
                std::vector<std::uint8_t> buf(wire::kMaxMessageLen, 0);
                const auto res = encode(c.msg, std::span<std::uint8_t>{buf});
                RC_ASSERT(res.is_err());
                RC_ASSERT(res.error().kind == c.expected);
            } else {
                // Decode of a malformed byte sequence. The input is captured
                // first so we can confirm decode() never mutates the caller's
                // bytes (state preserved on insufficient length; no message
                // produced for any malformed input).
                const std::vector<std::uint8_t> before = c.bytes;
                const auto res =
                    decode(std::span<const std::uint8_t>{c.bytes});
                RC_ASSERT(res.is_err());
                RC_ASSERT(res.error().kind == c.expected);
                RC_ASSERT(c.bytes == before);
            }
        });
    CHECK(ok);
}
