// Unit tests for the Binary_Codec encode/decode.
//
// These are example-based tests covering exact byte layout (byte-for-byte
// agreement with the Rust codec), encode/decode round trips, and the
// decode/encode error conditions. The exhaustive input-varying coverage lives
// in the property tests.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;

namespace {

// Encode a message into a freshly sized buffer, asserting success, and return
// the exact bytes written.
std::vector<std::uint8_t> encode_ok(const BinaryMessage& msg) {
    std::vector<std::uint8_t> buf(encoded_len_of(msg), 0);
    auto res = encode(msg, std::span<std::uint8_t>{buf});
    REQUIRE(res.is_ok());
    CHECK(res.value() == encoded_len_of(msg));
    return buf;
}

}  // namespace

TEST_CASE("NewOrder encodes to the exact little-endian fixed layout",
          "[codec][encode][layout]") {
    // order_id=42, Sell, price_ticks=12345 (0x3039), quantity=100 (0x64), seq=7
    BinaryMessage msg = NewOrder{42, Side::Sell, 12345, 100, 7};
    auto bytes = encode_ok(msg);

    REQUIRE(bytes.size() == 30);
    std::array<std::uint8_t, 30> expected = {
        0x00,                                            // type = NewOrder
        0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // order_id = 42
        0x01,                                            // side = Sell
        0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // price_ticks = 12345
        0x64, 0x00, 0x00, 0x00,                          // quantity = 100
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // seq = 7
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(bytes[i] == expected[i]);
    }
}

TEST_CASE("CancelOrder / Trade / Ack / Reject encode to their fixed lengths",
          "[codec][encode][layout]") {
    CHECK(encode_ok(BinaryMessage{CancelOrder{7}}).size() == 9);
    CHECK(encode_ok(BinaryMessage{Trade{1, 999, 50, 42, 17}}).size() == 37);
    CHECK(encode_ok(BinaryMessage{Ack{5, AckKind::Cancelled}}).size() == 10);
    CHECK(encode_ok(BinaryMessage{Reject{9, RejectReason::NoLongerResting}})
              .size() == 10);

    // Type discriminator is always the leading byte.
    CHECK(encode_ok(BinaryMessage{CancelOrder{7}})[0] == 1);
    CHECK(encode_ok(BinaryMessage{Trade{1, 999, 50, 42, 17}})[0] == 2);
    CHECK(encode_ok(BinaryMessage{Ack{5, AckKind::Cancelled}})[0] == 3);
    CHECK(encode_ok(BinaryMessage{Reject{9, RejectReason::InvalidPrice}})[0] == 4);
}

TEST_CASE("encode/decode round trip preserves every field for all types",
          "[codec][roundtrip]") {
    std::vector<BinaryMessage> messages = {
        NewOrder{1, Side::Buy, limits::kMinPriceTicks, limits::kMinGatewayQuantity, 0},
        NewOrder{~0ULL, Side::Sell, limits::kMaxPriceTicks,
                 limits::kMaxGatewayQuantity, ~0ULL},
        CancelOrder{123456789},
        Trade{7, 50000, 250, 11, 22},
        Ack{5, AckKind::Accepted},
        Ack{6, AckKind::Cancelled},
        Reject{9, RejectReason::InvalidPrice},
        Reject{10, RejectReason::IntegrityViolation},
    };

    for (const auto& original : messages) {
        auto bytes = encode_ok(original);
        auto decoded = decode(std::span<const std::uint8_t>{bytes});
        REQUIRE(decoded.is_ok());
        CHECK(decoded.value() == original);
    }
}

TEST_CASE("decode of an unknown type byte is rejected", "[codec][decode][error]") {
    std::array<std::uint8_t, 9> bytes{};
    bytes[0] = 200;  // not a known MessageType
    auto res = decode(std::span<const std::uint8_t>{bytes});
    REQUIRE(res.is_err());
    CHECK(res.error().kind == CodecErrorKind::UnknownType);
    CHECK(res.error().unknown_type == 200);
}

TEST_CASE("decode of a too-short byte sequence reports insufficient length",
          "[codec][decode][error]") {
    // A CancelOrder needs 9 bytes; give it only 4 (type byte present).
    std::array<std::uint8_t, 4> bytes{};
    bytes[0] = wire::message_type_as_byte(wire::MessageType::CancelOrder);
    auto res = decode(std::span<const std::uint8_t>{bytes});
    REQUIRE(res.is_err());
    CHECK(res.error().kind == CodecErrorKind::InsufficientLength);

    // An empty sequence has no type byte and is likewise insufficient.
    std::array<std::uint8_t, 0> empty{};
    auto empty_res = decode(std::span<const std::uint8_t>{empty});
    REQUIRE(empty_res.is_err());
    CHECK(empty_res.error().kind == CodecErrorKind::InsufficientLength);
}

TEST_CASE("decode of an over-long byte sequence reports excess trailing bytes",
          "[codec][decode][error]") {
    // A CancelOrder needs exactly 9 bytes; give it 10.
    std::array<std::uint8_t, 10> bytes{};
    bytes[0] = wire::message_type_as_byte(wire::MessageType::CancelOrder);
    auto res = decode(std::span<const std::uint8_t>{bytes});
    REQUIRE(res.is_err());
    CHECK(res.error().kind == CodecErrorKind::ExcessTrailingBytes);
}

TEST_CASE("decode of an invalid enum discriminator is field-out-of-range",
          "[codec][decode][error]") {
    auto ack = encode_ok(BinaryMessage{Ack{5, AckKind::Accepted}});
    ack[9] = 9;  // invalid AckKind discriminator
    auto res = decode(std::span<const std::uint8_t>{ack});
    REQUIRE(res.is_err());
    CHECK(res.error().kind == CodecErrorKind::FieldOutOfRange);
}

TEST_CASE("encode rejects out-of-range price and quantity fields",
          "[codec][encode][error]") {
    std::array<std::uint8_t, wire::kMaxMessageLen> buf{};

    auto bad_price = encode(BinaryMessage{NewOrder{1, Side::Buy, 0, 100, 0}},
                            std::span<std::uint8_t>{buf});
    REQUIRE(bad_price.is_err());
    CHECK(bad_price.error().kind == CodecErrorKind::FieldOutOfRange);

    auto bad_qty = encode(
        BinaryMessage{NewOrder{1, Side::Buy, limits::kMinPriceTicks, 0, 0}},
        std::span<std::uint8_t>{buf});
    REQUIRE(bad_qty.is_err());
    CHECK(bad_qty.error().kind == CodecErrorKind::FieldOutOfRange);
}

TEST_CASE("encode into a too-small buffer reports insufficient length",
          "[codec][encode][error]") {
    std::array<std::uint8_t, 5> tiny{};  // smaller than any message
    auto res = encode(BinaryMessage{CancelOrder{7}}, std::span<std::uint8_t>{tiny});
    REQUIRE(res.is_err());
    CHECK(res.error().kind == CodecErrorKind::InsufficientLength);
}

TEST_CASE("decode then re-encode reproduces the original bytes",
          "[codec][roundtrip][bytes]") {
    BinaryMessage msg = Trade{7, 50000, 250, 11, 22};
    auto original = encode_ok(msg);

    auto decoded = decode(std::span<const std::uint8_t>{original});
    REQUIRE(decoded.is_ok());

    std::vector<std::uint8_t> reencoded(original.size(), 0);
    auto res = encode(decoded.value(), std::span<std::uint8_t>{reencoded});
    REQUIRE(res.is_ok());
    CHECK(reencoded == original);
}
