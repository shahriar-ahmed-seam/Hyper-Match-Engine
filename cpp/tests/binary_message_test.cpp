// Unit tests for the Binary_Message structs, fixed per-type byte lengths, and
// the typed CodecError / CodecResult vocabulary.
//
// These are definition-level tests only - encode/decode behavior is covered by
// the codec tests and property tests. They confirm the C++ message model
// mirrors the Rust `BinaryMessage` / `CodecError` shape and that the variant
// index agrees with the Wire_Protocol type byte.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <variant>

#include "hme/binary_message.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;

TEST_CASE("BinaryMessage variant index equals its Wire_Protocol type byte",
          "[codec][binary_message]") {
    CHECK(message_type_of(BinaryMessage{NewOrder{}}) == wire::MessageType::NewOrder);
    CHECK(message_type_of(BinaryMessage{CancelOrder{}}) == wire::MessageType::CancelOrder);
    CHECK(message_type_of(BinaryMessage{Trade{}}) == wire::MessageType::Trade);
    CHECK(message_type_of(BinaryMessage{Ack{}}) == wire::MessageType::Ack);
    CHECK(message_type_of(BinaryMessage{Reject{}}) == wire::MessageType::Reject);
}

TEST_CASE("Each Binary_Message variant reports its fixed encoded length",
          "[codec][binary_message][layout]") {
    CHECK(encoded_len_of(BinaryMessage{NewOrder{}}) == 30);
    CHECK(encoded_len_of(BinaryMessage{CancelOrder{}}) == 9);
    CHECK(encoded_len_of(BinaryMessage{Trade{}}) == 37);
    CHECK(encoded_len_of(BinaryMessage{Ack{}}) == 10);
    CHECK(encoded_len_of(BinaryMessage{Reject{}}) == 10);
}

TEST_CASE("Binary_Message structs carry all Wire_Protocol fields and compare by value",
          "[codec][binary_message][fields]") {
    NewOrder a{42, Side::Sell, 12345, 100, 7};
    NewOrder b{42, Side::Sell, 12345, 100, 7};
    CHECK(a == b);
    b.seq = 8;
    CHECK_FALSE(a == b);

    Trade t{1, 999, 50, 42, 17};
    CHECK(t.exec_seq == 1);
    CHECK(t.price_ticks == 999);
    CHECK(t.quantity == 50);
    CHECK(t.incoming_id == 42);
    CHECK(t.resting_id == 17);

    Ack ack{5, AckKind::Cancelled};
    CHECK(ack == Ack{5, AckKind::Cancelled});

    Reject r{9, RejectReason::NoLongerResting};
    CHECK(r == Reject{9, RejectReason::NoLongerResting});

    CHECK(CancelOrder{3} == CancelOrder{3});
}

TEST_CASE("CodecError named constructors mirror the Rust CodecError variants",
          "[codec][error]") {
    auto unknown = CodecError::unknown_type_error(200);
    CHECK(unknown.kind == CodecErrorKind::UnknownType);
    CHECK(unknown.unknown_type == 200);

    auto insufficient = CodecError::insufficient_length();
    CHECK(insufficient.kind == CodecErrorKind::InsufficientLength);

    auto excess = CodecError::excess_trailing_bytes();
    CHECK(excess.kind == CodecErrorKind::ExcessTrailingBytes);

    auto oor = CodecError::field_out_of_range("price_ticks");
    CHECK(oor.kind == CodecErrorKind::FieldOutOfRange);
    CHECK(std::string_view{oor.field} == "price_ticks");

    CHECK(insufficient == CodecError::insufficient_length());
    CHECK_FALSE(insufficient == excess);
}

TEST_CASE("CodecResult holds either a value or a typed error", "[codec][result]") {
    CodecResult<std::size_t> ok{wire::kTradeLen};
    REQUIRE(ok.is_ok());
    CHECK_FALSE(ok.is_err());
    CHECK(static_cast<bool>(ok));
    CHECK(ok.value() == wire::kTradeLen);

    CodecResult<BinaryMessage> err{CodecError::unknown_type_error(7)};
    REQUIRE(err.is_err());
    CHECK_FALSE(err.is_ok());
    CHECK_FALSE(static_cast<bool>(err));
    CHECK(err.error().kind == CodecErrorKind::UnknownType);
    CHECK(err.error().unknown_type == 7);

    CodecResult<BinaryMessage> ok_msg{BinaryMessage{Ack{1, AckKind::Accepted}}};
    REQUIRE(ok_msg.is_ok());
    CHECK(std::get<Ack>(ok_msg.value()) == Ack{1, AckKind::Accepted});
}
