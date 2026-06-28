// Unit tests for the Network_Server connection-lifecycle state machine.
//
// These example-based tests cover the event-loop lifecycle decisions, exercised
// through ConnectionRegistry with a fake readiness poller so the logic is
// deterministic and OS-independent:
//   - accept + register a client socket for readiness
//   - reject past the concurrent-connection cap and release its resources,
//     without registering
//   - a single teardown path -- close, release, deregister -- shared by
//     oversize frames, peer close, and socket errors
//
// The 100 ms release bound and real OS socket behavior are validated by the
// integration tests.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/connection_registry.hpp"
#include "hme/network.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;
using hme::network::AcceptResult;
using hme::network::ConnectionRegistry;
using hme::network::ReadinessPoller;
using hme::network::SocketHandle;
using hme::network::TeardownReason;

namespace {

// A fake ReadinessPoller that records every adapter call in order, so tests can
// assert exactly which OS-level actions the registry drives (register /
// deregister / close) and on which sockets.
struct FakePoller : ReadinessPoller {
    enum class Op : std::uint8_t { Register, Deregister, Close };
    struct Call {
        Op op;
        SocketHandle fd;
        friend bool operator==(const Call&, const Call&) = default;
    };

    std::vector<Call> calls;

    void register_readiness(SocketHandle fd) override {
        calls.push_back({Op::Register, fd});
    }
    void deregister_readiness(SocketHandle fd) override {
        calls.push_back({Op::Deregister, fd});
    }
    void close_socket(SocketHandle fd) override {
        calls.push_back({Op::Close, fd});
    }

    std::size_t count(Op op) const {
        std::size_t n = 0;
        for (const auto& c : calls) {
            if (c.op == op) ++n;
        }
        return n;
    }
};

// Append the wire-encoding of `msg` to `out`.
void append_encoded(std::vector<std::uint8_t>& out, const BinaryMessage& msg) {
    const std::size_t at = out.size();
    out.resize(at + encoded_len_of(msg));
    auto res = encode(msg, std::span<std::uint8_t>{out.data() + at,
                                                   encoded_len_of(msg)});
    REQUIRE(res.is_ok());
}

// A deliver sink recording delivered Binary_Messages in order.
struct Sink {
    std::vector<BinaryMessage> messages;
    void operator()(const BinaryMessage& m) { messages.push_back(m); }
};

}  // namespace

TEST_CASE("accept registers a client socket for readiness", "[registry][accept]") {
    // Accept the connection and register it for readiness.
    FakePoller poller;
    ConnectionRegistry reg(poller);

    CHECK(reg.accept(7) == AcceptResult::Registered);

    CHECK(reg.active_count() == 1);
    CHECK(reg.is_registered(7));
    REQUIRE(poller.calls.size() == 1);
    CHECK(poller.calls[0] == FakePoller::Call{FakePoller::Op::Register, 7});
}

TEST_CASE("accept registers multiple distinct sockets", "[registry][accept]") {
    FakePoller poller;
    ConnectionRegistry reg(poller);

    CHECK(reg.accept(1) == AcceptResult::Registered);
    CHECK(reg.accept(2) == AcceptResult::Registered);
    CHECK(reg.accept(3) == AcceptResult::Registered);

    CHECK(reg.active_count() == 3);
    CHECK(poller.count(FakePoller::Op::Register) == 3);
    CHECK(poller.count(FakePoller::Op::Close) == 0);
}

TEST_CASE("accept past the connection cap is rejected and released",
          "[registry][cap]") {
    // Accepting beyond the cap rejects the connection and releases the
    // resources allocated for it, without registering.
    FakePoller poller;
    ConnectionRegistry reg(poller, /*max_connections=*/3);

    REQUIRE(reg.accept(10) == AcceptResult::Registered);
    REQUIRE(reg.accept(11) == AcceptResult::Registered);
    REQUIRE(reg.accept(12) == AcceptResult::Registered);
    CHECK(reg.active_count() == 3);

    // The 4th connection exceeds the cap of 3.
    CHECK(reg.accept(13) == AcceptResult::RejectedAtCapacity);

    // The rejected socket was closed (released) but never registered, and the
    // tracked set is unchanged.
    CHECK(reg.active_count() == 3);
    CHECK_FALSE(reg.is_registered(13));
    CHECK(poller.count(FakePoller::Op::Register) == 3);  // not 4
    CHECK(poller.calls.back() ==
          FakePoller::Call{FakePoller::Op::Close, 13});
}

TEST_CASE("the real cap is 10,000 concurrent connections", "[registry][cap]") {
    // The default cap matches the 10,000 limit constant.
    FakePoller poller;
    ConnectionRegistry reg(poller);
    CHECK(reg.max_connections() == hme::network::kMaxConnections);
    CHECK(reg.max_connections() == 10000);
}

TEST_CASE("a freed slot lets a new connection be accepted", "[registry][cap]") {
    // After teardown frees a slot, a previously-rejected connection can be
    // accepted (the cap counts live connections).
    FakePoller poller;
    ConnectionRegistry reg(poller, /*max_connections=*/2);

    REQUIRE(reg.accept(1) == AcceptResult::Registered);
    REQUIRE(reg.accept(2) == AcceptResult::Registered);
    CHECK(reg.accept(3) == AcceptResult::RejectedAtCapacity);

    reg.teardown(1, TeardownReason::PeerClosed);
    CHECK(reg.active_count() == 1);

    CHECK(reg.accept(3) == AcceptResult::Registered);
    CHECK(reg.active_count() == 2);
}

TEST_CASE("teardown closes, releases, and deregisters in one path",
          "[registry][teardown]") {
    // A single teardown path stops monitoring readiness and closes/releases the
    // socket.
    FakePoller poller;
    ConnectionRegistry reg(poller);
    REQUIRE(reg.accept(5) == AcceptResult::Registered);
    poller.calls.clear();

    reg.teardown(5, TeardownReason::SocketError);

    CHECK(reg.active_count() == 0);
    CHECK_FALSE(reg.is_registered(5));
    // Deregister readiness then close the socket.
    REQUIRE(poller.calls.size() == 2);
    CHECK(poller.calls[0] ==
          FakePoller::Call{FakePoller::Op::Deregister, 5});
    CHECK(poller.calls[1] == FakePoller::Call{FakePoller::Op::Close, 5});
}

TEST_CASE("peer close and socket error use the same teardown path",
          "[registry][teardown]") {
    // Peer close and socket error funnel through the identical teardown
    // sequence.
    auto teardown_calls = [](TeardownReason reason) {
        FakePoller poller;
        ConnectionRegistry reg(poller);
        REQUIRE(reg.accept(9) == AcceptResult::Registered);
        poller.calls.clear();
        reg.teardown(9, reason);
        return poller.calls;
    };

    const auto peer = teardown_calls(TeardownReason::PeerClosed);
    const auto err = teardown_calls(TeardownReason::SocketError);
    const auto oversize = teardown_calls(TeardownReason::OversizeFrame);

    CHECK(peer == err);
    CHECK(peer == oversize);
}

TEST_CASE("teardown is idempotent for an unknown or repeated socket",
          "[registry][teardown]") {
    FakePoller poller;
    ConnectionRegistry reg(poller);
    REQUIRE(reg.accept(8) == AcceptResult::Registered);

    reg.teardown(8, TeardownReason::PeerClosed);
    poller.calls.clear();

    // Tearing down again, or an fd that was never tracked, is a safe no-op.
    reg.teardown(8, TeardownReason::SocketError);
    reg.teardown(999, TeardownReason::SocketError);

    CHECK(reg.active_count() == 0);
    CHECK(poller.calls.empty());
}

TEST_CASE("on_readable delivers complete frames on a registered socket",
          "[registry][read]") {
    // Deliver each complete Binary_Message to the consumer.
    FakePoller poller;
    ConnectionRegistry reg(poller);
    REQUIRE(reg.accept(4) == AcceptResult::Registered);

    std::vector<BinaryMessage> expected = {
        NewOrder{1, Side::Buy, 200, 10, 0},
        CancelOrder{2},
    };
    std::vector<std::uint8_t> stream;
    for (const auto& m : expected) append_encoded(stream, m);

    Sink sink;
    CHECK(reg.on_readable(4, std::span<const std::uint8_t>{stream}, sink));

    REQUIRE(sink.messages.size() == 2);
    CHECK(sink.messages[0] == expected[0]);
    CHECK(sink.messages[1] == expected[1]);
    CHECK(reg.is_registered(4));  // still open
}

TEST_CASE("on_readable retains a partial frame across reads", "[registry][read]") {
    // Buffer partial bytes per connection until the rest arrive.
    FakePoller poller;
    ConnectionRegistry reg(poller);
    REQUIRE(reg.accept(4) == AcceptResult::Registered);

    std::vector<std::uint8_t> stream;
    BinaryMessage msg = Trade{7, 50000, 250, 11, 22};  // 37 bytes
    append_encoded(stream, msg);

    Sink sink;
    CHECK(reg.on_readable(4, std::span<const std::uint8_t>{stream.data(), 10},
                          sink));
    CHECK(sink.messages.empty());
    CHECK(reg.buffered(4) == 10);

    CHECK(reg.on_readable(
        4, std::span<const std::uint8_t>{stream.data() + 10, stream.size() - 10},
        sink));
    REQUIRE(sink.messages.size() == 1);
    CHECK(sink.messages[0] == msg);
    CHECK(reg.buffered(4) == 0);
}

TEST_CASE("on_readable tears down on an oversize/undecodable frame",
          "[registry][read][teardown]") {
    // An undecodable frame closes the connection through the single teardown
    // path -- close + release + deregister.
    FakePoller poller;
    ConnectionRegistry reg(poller);
    REQUIRE(reg.accept(6) == AcceptResult::Registered);
    poller.calls.clear();

    std::array<std::uint8_t, 4> garbage{200, 0, 0, 0};  // unknown type byte
    CHECK_FALSE(
        reg.on_readable(6, std::span<const std::uint8_t>{garbage}, [](auto&) {}));

    CHECK(reg.active_count() == 0);
    CHECK_FALSE(reg.is_registered(6));
    REQUIRE(poller.calls.size() == 2);
    CHECK(poller.calls[0] ==
          FakePoller::Call{FakePoller::Op::Deregister, 6});
    CHECK(poller.calls[1] == FakePoller::Call{FakePoller::Op::Close, 6});
}

TEST_CASE("on_readable on an unknown socket is a no-op", "[registry][read]") {
    FakePoller poller;
    ConnectionRegistry reg(poller);

    Sink sink;
    CHECK_FALSE(
        reg.on_readable(123, std::span<const std::uint8_t>{}, sink));
    CHECK(sink.messages.empty());
    CHECK(poller.calls.empty());
}
