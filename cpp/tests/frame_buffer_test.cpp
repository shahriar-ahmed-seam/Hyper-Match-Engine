// Unit tests for the per-connection framing buffer.
//
// These example-based tests cover the framing contract:
//   - read available bytes and deliver each complete Binary_Message
//   - retain a partial frame across reads without losing bytes
//   - close the connection on an undecodable/oversize frame without growing
//     the buffer past the 65,536-byte cap
//
// The exhaustive input-varying coverage (arbitrary chunk splits and oversize
// rejection) lives in the RapidCheck property tests.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/frame_buffer.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;
using hme::network::FrameBuffer;
using hme::network::FrameStatus;
using hme::network::kMaxMessageSize;

namespace {

// Append the wire-encoding of `msg` to `out`.
void append_encoded(std::vector<std::uint8_t>& out, const BinaryMessage& msg) {
    const std::size_t at = out.size();
    out.resize(at + encoded_len_of(msg));
    auto res = encode(msg, std::span<std::uint8_t>{out.data() + at,
                                                   encoded_len_of(msg)});
    REQUIRE(res.is_ok());
}

// A deliver sink that records every delivered Binary_Message in order.
struct Sink {
    std::vector<BinaryMessage> messages;
    void operator()(const BinaryMessage& m) { messages.push_back(m); }
};

}  // namespace

TEST_CASE("FrameBuffer delivers a single complete message", "[frame]") {
    std::vector<std::uint8_t> stream;
    BinaryMessage msg = NewOrder{42, Side::Sell, 12345, 100, 7};
    append_encoded(stream, msg);

    FrameBuffer fb;
    Sink sink;
    auto status = fb.consume(std::span<const std::uint8_t>{stream}, sink);

    CHECK(status == FrameStatus::Ok);
    REQUIRE(sink.messages.size() == 1);
    CHECK(sink.messages[0] == msg);
    CHECK(fb.buffered() == 0);
    CHECK_FALSE(fb.closed());
}

TEST_CASE("FrameBuffer delivers several concatenated messages in order",
          "[frame]") {
    std::vector<BinaryMessage> expected = {
        NewOrder{1, Side::Buy, limits::kMinPriceTicks, 10, 0},
        CancelOrder{2},
        Trade{3, 50000, 25, 1, 99},
        Ack{4, AckKind::Accepted},
        Reject{5, RejectReason::OrderNotFound},
    };

    std::vector<std::uint8_t> stream;
    for (const auto& m : expected) {
        append_encoded(stream, m);
    }

    FrameBuffer fb;
    Sink sink;
    auto status = fb.consume(std::span<const std::uint8_t>{stream}, sink);

    CHECK(status == FrameStatus::Ok);
    REQUIRE(sink.messages.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(sink.messages[i] == expected[i]);
    }
    CHECK(fb.buffered() == 0);
}

TEST_CASE("FrameBuffer reassembles a message split across two reads",
          "[frame][partial]") {
    std::vector<std::uint8_t> stream;
    BinaryMessage msg = Trade{7, 50000, 250, 11, 22};  // 37 bytes
    append_encoded(stream, msg);

    FrameBuffer fb;
    Sink sink;

    // First read carries only the first 10 bytes -> nothing delivered yet, the
    // partial frame is retained.
    auto s1 = fb.consume(std::span<const std::uint8_t>{stream.data(), 10}, sink);
    CHECK(s1 == FrameStatus::Ok);
    CHECK(sink.messages.empty());
    CHECK(fb.buffered() == 10);

    // Second read carries the remaining bytes -> the message is delivered.
    auto s2 = fb.consume(
        std::span<const std::uint8_t>{stream.data() + 10, stream.size() - 10},
        sink);
    CHECK(s2 == FrameStatus::Ok);
    REQUIRE(sink.messages.size() == 1);
    CHECK(sink.messages[0] == msg);
    CHECK(fb.buffered() == 0);
}

TEST_CASE("FrameBuffer reassembles correctly when fed one byte at a time",
          "[frame][partial]") {
    std::vector<BinaryMessage> expected = {
        NewOrder{1, Side::Buy, 200, 10, 0},
        CancelOrder{2},
        Ack{3, AckKind::Cancelled},
    };
    std::vector<std::uint8_t> stream;
    for (const auto& m : expected) {
        append_encoded(stream, m);
    }

    FrameBuffer fb;
    Sink sink;
    for (std::uint8_t byte : stream) {
        std::array<std::uint8_t, 1> one{byte};
        REQUIRE(fb.consume(std::span<const std::uint8_t>{one}, sink) ==
                FrameStatus::Ok);
        CHECK(fb.buffered() <= wire::kMaxMessageLen);
    }

    REQUIRE(sink.messages.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(sink.messages[i] == expected[i]);
    }
    CHECK(fb.buffered() == 0);
}

TEST_CASE("FrameBuffer retains a trailing partial after delivering whole frames",
          "[frame][partial]") {
    std::vector<std::uint8_t> stream;
    BinaryMessage whole = CancelOrder{7};       // 9 bytes
    BinaryMessage trailing = NewOrder{8, Side::Buy, 300, 5, 1};  // 30 bytes
    append_encoded(stream, whole);
    append_encoded(stream, trailing);

    // Feed the whole CancelOrder plus only the first 4 bytes of the NewOrder.
    FrameBuffer fb;
    Sink sink;
    auto status = fb.consume(
        std::span<const std::uint8_t>{stream.data(), 9 + 4}, sink);

    CHECK(status == FrameStatus::Ok);
    REQUIRE(sink.messages.size() == 1);
    CHECK(sink.messages[0] == whole);
    CHECK(fb.buffered() == 4);  // the 4 retained bytes of the next frame

    // Deliver the rest of the NewOrder.
    auto rest = fb.consume(
        std::span<const std::uint8_t>{stream.data() + 13, stream.size() - 13},
        sink);
    CHECK(rest == FrameStatus::Ok);
    REQUIRE(sink.messages.size() == 2);
    CHECK(sink.messages[1] == trailing);
    CHECK(fb.buffered() == 0);
}

TEST_CASE("FrameBuffer signals closure on an unknown type byte", "[frame][close]") {
    std::array<std::uint8_t, 4> garbage{200, 0, 0, 0};  // 200 is no known type

    FrameBuffer fb;
    Sink sink;
    auto status = fb.consume(std::span<const std::uint8_t>{garbage}, sink);

    CHECK(status == FrameStatus::Closed);
    CHECK(fb.closed());
    CHECK(sink.messages.empty());
    // The buffer is never grown past the cap on closure.
    CHECK(fb.buffered() <= kMaxMessageSize);

    // Once closed, further reads are no-ops returning Closed.
    std::array<std::uint8_t, 1> more{0};
    CHECK(fb.consume(std::span<const std::uint8_t>{more}, sink) ==
          FrameStatus::Closed);
    CHECK(sink.messages.empty());
}

TEST_CASE("FrameBuffer signals closure on an undecodable in-range frame",
          "[frame][close]") {
    // A correctly-sized Ack frame whose AckKind discriminator is invalid does
    // not decode, so the frame is undecodable and the connection is closed.
    std::vector<std::uint8_t> stream;
    append_encoded(stream, BinaryMessage{Ack{5, AckKind::Accepted}});
    stream[9] = 9;  // invalid AckKind byte

    FrameBuffer fb;
    Sink sink;
    auto status = fb.consume(std::span<const std::uint8_t>{stream}, sink);

    CHECK(status == FrameStatus::Closed);
    CHECK(fb.closed());
    CHECK(sink.messages.empty());
}

TEST_CASE("FrameBuffer delivers good frames before detecting a bad one",
          "[frame][close]") {
    std::vector<std::uint8_t> stream;
    BinaryMessage good = CancelOrder{11};
    append_encoded(stream, good);
    const std::size_t bad_at = stream.size();
    stream.push_back(200);  // unknown type byte begins the next frame
    stream.push_back(0);

    FrameBuffer fb;
    Sink sink;
    auto status = fb.consume(std::span<const std::uint8_t>{stream}, sink);

    CHECK(status == FrameStatus::Closed);
    REQUIRE(sink.messages.size() == 1);
    CHECK(sink.messages[0] == good);
    CHECK(bad_at == 9);
}
