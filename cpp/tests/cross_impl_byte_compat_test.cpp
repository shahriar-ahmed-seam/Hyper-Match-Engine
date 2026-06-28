// Cross-implementation byte-compatibility test (C++ side).
//
// Asserts the C++ Binary_Codec and the Rust Binary_Codec encode the same
// Binary_Messages to byte-identical sequences.
//
// Why golden vectors instead of a two-toolchain harness:
// Driving the Rust and C++ encoders from one process would require an FFI/IPC
// bridge and a combined Rust+C++ build, which is fragile and slow. Instead we
// define ONE fixed set of representative message vectors together with their
// EXPECTED golden byte arrays (hand-computed from the Wire_Protocol: a leading
// little-endian type byte followed by little-endian fixed-width fields). This
// file asserts `C++ encode(vector) == golden`. The mirror file
// `codec/tests/cross_impl_byte_compat.rs` asserts `Rust encode(vector) == the
// SAME golden bytes`. Because both implementations are checked against the
// identical hand-computed golden vectors, byte-for-byte agreement between the
// two implementations follows transitively (C++ == golden == Rust).
//
// The two files MUST be kept in lock step: the vectors and golden bytes here
// are duplicated verbatim from the Rust mirror. Each vector covers a distinct
// part of the layout (every message type, both Sides, both AckKinds, several
// RejectReasons, and the price/quantity/order-id boundary values).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"

using namespace hme;

namespace {

struct Vector {
    const char* name;
    BinaryMessage msg;
    std::vector<std::uint8_t> golden;
};

// The shared, fixed set of representative vectors. These exact messages and
// golden bytes are mirrored byte-for-byte in the Rust test
// (codec/tests/cross_impl_byte_compat.rs).
std::vector<Vector> vectors() {
    return {
        // 1) NewOrder, minimum in-range values, Buy.
        {"NewOrder min/Buy",
         BinaryMessage{NewOrder{1, Side::Buy, 1, 1, 0}},
         {
             0x00,                                            // type = NewOrder
             0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // order_id = 1
             0x00,                                            // side = Buy
             0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // price_ticks = 1
             0x01, 0x00, 0x00, 0x00,                          // quantity = 1
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // seq = 0
         }},
        // 2) NewOrder, mixed multi-byte values, Sell.
        {"NewOrder mixed/Sell",
         BinaryMessage{NewOrder{0xDEADBEEFULL, Side::Sell, 12345, 678,
                                0x0102030405060708ULL}},
         {
             0x00,                                            // type = NewOrder
             0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00,  // order_id = 0xDEADBEEF
             0x01,                                            // side = Sell
             0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // price_ticks = 12345
             0xA6, 0x02, 0x00, 0x00,                          // quantity = 678
             0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // seq = 0x0102030405060708
         }},
        // 3) NewOrder, maximum in-range values, Sell.
        {"NewOrder max/Sell",
         BinaryMessage{NewOrder{0xFFFFFFFFFFFFFFFFULL, Side::Sell,
                                99999999999ULL, 1000000000U,
                                0xFFFFFFFFFFFFFFFFULL}},
         {
             0x00,                                            // type = NewOrder
             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // order_id = u64 max
             0x01,                                            // side = Sell
             0xFF, 0xE7, 0x76, 0x48, 0x17, 0x00, 0x00, 0x00,  // price_ticks = 99,999,999,999
             0x00, 0xCA, 0x9A, 0x3B,                          // quantity = 1,000,000,000
             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // seq = u64 max
         }},
        // 4) CancelOrder, maximum order_id.
        {"CancelOrder max id",
         BinaryMessage{CancelOrder{0xFFFFFFFFFFFFFFFFULL}},
         {
             0x01,                                            // type = CancelOrder
             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // order_id = u64 max
         }},
        // 5) CancelOrder, mid-range order_id.
        {"CancelOrder mid id",
         BinaryMessage{CancelOrder{123456789ULL}},
         {
             0x01,                                            // type = CancelOrder
             0x15, 0xCD, 0x5B, 0x07, 0x00, 0x00, 0x00, 0x00,  // order_id = 123456789
         }},
        // 6) Trade, mixed values.
        {"Trade",
         BinaryMessage{Trade{7, 50000, 250, 11, 22}},
         {
             0x02,                                            // type = Trade
             0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // exec_seq = 7
             0x50, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // price_ticks = 50000
             0xFA, 0x00, 0x00, 0x00,                          // quantity = 250
             0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // incoming_id = 11
             0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // resting_id = 22
         }},
        // 7) Ack, Accepted.
        {"Ack accepted",
         BinaryMessage{Ack{5, AckKind::Accepted}},
         {
             0x03,                                            // type = Ack
             0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // order_id = 5
             0x00,                                            // kind = Accepted
         }},
        // 8) Ack, Cancelled.
        {"Ack cancelled",
         BinaryMessage{Ack{6, AckKind::Cancelled}},
         {
             0x03,                                            // type = Ack
             0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // order_id = 6
             0x01,                                            // kind = Cancelled
         }},
        // 9) Reject, NoLongerResting.
        {"Reject no-longer-resting",
         BinaryMessage{Reject{8, RejectReason::NoLongerResting}},
         {
             0x04,                                            // type = Reject
             0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // order_id = 8
             0x03,                                            // reason = NoLongerResting
         }},
        // 10) Reject, IntegrityViolation.
        {"Reject integrity-violation",
         BinaryMessage{Reject{10, RejectReason::IntegrityViolation}},
         {
             0x04,                                            // type = Reject
             0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // order_id = 10
             0x04,                                            // reason = IntegrityViolation
         }},
    };
}

}  // namespace

TEST_CASE("C++ encode matches the shared cross-impl golden vectors",
          "[codec][cross-impl][bytes]") {
    for (const auto& v : vectors()) {
        INFO("vector: " << v.name);
        std::vector<std::uint8_t> buf(encoded_len_of(v.msg), 0);
        auto res = encode(v.msg, std::span<std::uint8_t>{buf});
        REQUIRE(res.is_ok());
        REQUIRE(res.value() == v.golden.size());
        // A mismatch here means the C++ codec disagrees with the shared golden
        // bytes; since the Rust codec is checked against the same golden bytes,
        // that would mean the two implementations disagree byte-for-byte.
        CHECK(buf == v.golden);
    }
}
