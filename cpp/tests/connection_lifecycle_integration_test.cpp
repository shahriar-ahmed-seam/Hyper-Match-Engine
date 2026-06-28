// Integration tests for the Network_Server connection lifecycle.
//
// Where connection_registry_test.cpp checks individual lifecycle decisions with
// a call-recording fake, these tests drive the ConnectionRegistry through full
// end-to-end lifecycle SEQUENCES against an in-memory poller that actually
// MODELS socket-resource ownership: a socket is "OS-opened" (as accept(2) would
// do) before being offered to the registry, register/deregister flip a
// readiness-monitoring flag, and close releases the OS resource. The poller
// flags resource bugs (double close, closing an unopened socket, monitoring a
// closed socket) and leaks (a socket still monitored after its resource is
// gone). That lets each test assert the connection ends in a fully clean state.
//
// Coverage:
//   - accept + register a connection end-to-end
//   - reject past the 10,000 cap and release without registering
//   - release within 100 ms on peer close (measured, steady clock)
//   - close via the same teardown path on a socket error

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <span>
#include <unordered_set>
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

// An in-memory ReadinessPoller that models real socket-resource lifecycle so
// integration tests can assert resources are actually acquired and released
// (not merely that the right calls were issued in the right order).
struct InMemoryPoller : ReadinessPoller {
    std::unordered_set<SocketHandle> open;        // OS-held socket resources
    std::unordered_set<SocketHandle> monitored;   // registered for readiness
    std::unordered_set<SocketHandle> ever_closed; // closed at least once

    std::size_t register_calls = 0;
    std::size_t deregister_calls = 0;
    std::size_t close_calls = 0;

    // Resource-misuse flags; all must stay false for a correct lifecycle.
    bool monitored_unopened = false;  // asked to monitor a non-open socket
    bool closed_unopened = false;     // asked to close a never-open socket
    bool double_closed = false;       // asked to close an already-closed socket

    // Simulate the OS accept(2)/AcceptEx that opens the socket before the
    // event loop hands it to the registry.
    void os_open(SocketHandle fd) { open.insert(fd); }

    void register_readiness(SocketHandle fd) override {
        if (open.find(fd) == open.end()) monitored_unopened = true;
        monitored.insert(fd);
        ++register_calls;
    }
    void deregister_readiness(SocketHandle fd) override {
        monitored.erase(fd);
        ++deregister_calls;
    }
    void close_socket(SocketHandle fd) override {
        if (open.find(fd) == open.end()) {
            if (ever_closed.find(fd) != ever_closed.end()) {
                double_closed = true;
            } else {
                closed_unopened = true;
            }
        }
        open.erase(fd);
        ever_closed.insert(fd);
        ++close_calls;
    }

    bool is_open(SocketHandle fd) const { return open.count(fd) != 0; }
    bool is_monitored(SocketHandle fd) const { return monitored.count(fd) != 0; }

    // True if any socket is still monitored for readiness whose OS resource has
    // already been released -- i.e. a dangling registration (a leak).
    bool has_dangling_monitor() const {
        for (SocketHandle fd : monitored) {
            if (open.find(fd) == open.end()) return true;
        }
        return false;
    }

    // No resource-misuse flag tripped.
    bool clean() const {
        return !monitored_unopened && !closed_unopened && !double_closed &&
               !has_dangling_monitor();
    }
};

// Append the wire-encoding of `msg` to `out`.
void append_encoded(std::vector<std::uint8_t>& out, const BinaryMessage& msg) {
    const std::size_t at = out.size();
    out.resize(at + encoded_len_of(msg));
    auto res = encode(
        msg, std::span<std::uint8_t>{out.data() + at, encoded_len_of(msg)});
    REQUIRE(res.is_ok());
}

// A deliver sink recording delivered Binary_Messages in arrival order.
struct Sink {
    std::vector<BinaryMessage> messages;
    void operator()(const BinaryMessage& m) { messages.push_back(m); }
};

}  // namespace

TEST_CASE("lifecycle: accept registers a connection and carries traffic",
          "[integration][lifecycle][accept]") {
    // A freshly accepted socket is registered for readiness and then exchanges
    // complete Binary_Messages while staying open.
    InMemoryPoller poller;
    ConnectionRegistry reg(poller);

    const SocketHandle fd = 42;
    poller.os_open(fd);  // the OS accepted the socket

    REQUIRE(reg.accept(fd) == AcceptResult::Registered);

    // The socket is now both tracked by the registry and monitored by the OS
    // adapter, and its resource is still held open.
    CHECK(reg.active_count() == 1);
    CHECK(reg.is_registered(fd));
    CHECK(poller.is_open(fd));
    CHECK(poller.is_monitored(fd));
    CHECK(poller.register_calls == 1);

    // Drive real traffic end-to-end: two whole messages arrive and are
    // delivered in order; the connection stays open.
    std::vector<BinaryMessage> expected = {
        NewOrder{1, Side::Buy, 200, 10, 0},
        CancelOrder{2},
    };
    std::vector<std::uint8_t> stream;
    for (const auto& m : expected) append_encoded(stream, m);

    Sink sink;
    CHECK(reg.on_readable(fd, std::span<const std::uint8_t>{stream}, sink));
    REQUIRE(sink.messages.size() == 2);
    CHECK(sink.messages[0] == expected[0]);
    CHECK(sink.messages[1] == expected[1]);

    CHECK(reg.is_registered(fd));
    CHECK(poller.is_open(fd));
    CHECK(poller.clean());
}

TEST_CASE("lifecycle: the default connection cap is 10,000",
          "[integration][lifecycle][cap]") {
    // The cap defaults to the 10,000 concurrent-connection limit when none is
    // injected.
    InMemoryPoller poller;
    ConnectionRegistry reg(poller);
    CHECK(reg.max_connections() == hme::network::kMaxConnections);
    CHECK(reg.max_connections() == 10000);
}

TEST_CASE("lifecycle: rejection at the real 10,000 cap releases the socket",
          "[integration][lifecycle][cap]") {
    // Fill to the actual default cap of 10,000, then the next accept is rejected
    // and its socket released without being registered.
    InMemoryPoller poller;
    ConnectionRegistry reg(poller);  // default cap == 10,000

    for (SocketHandle fd = 1; fd <= 10000; ++fd) {
        poller.os_open(fd);
        REQUIRE(reg.accept(fd) == AcceptResult::Registered);
    }
    REQUIRE(reg.active_count() == 10000);
    REQUIRE(poller.register_calls == 10000);

    // The 10,001st connection would exceed the cap.
    const SocketHandle over = 10001;
    poller.os_open(over);
    CHECK(reg.accept(over) == AcceptResult::RejectedAtCapacity);

    // It was released (closed), never registered, and the live set is intact.
    CHECK(reg.active_count() == 10000);
    CHECK_FALSE(reg.is_registered(over));
    CHECK_FALSE(poller.is_open(over));         // resource released
    CHECK_FALSE(poller.is_monitored(over));    // never monitored
    CHECK(poller.register_calls == 10000);     // not 10,001
    CHECK(poller.clean());
}

TEST_CASE("lifecycle: bulk rejection past the cap releases every excess socket",
          "[integration][lifecycle][cap]") {
    // With a smaller injected cap (so the bulk behavior is cheap to exercise),
    // every connection past the cap is rejected and each of their sockets is
    // released without registration -- no resource leak.
    constexpr std::uint32_t kCap = 8;
    InMemoryPoller poller;
    ConnectionRegistry reg(poller, kCap);

    // Fill to capacity.
    for (SocketHandle fd = 1; fd <= static_cast<SocketHandle>(kCap); ++fd) {
        poller.os_open(fd);
        REQUIRE(reg.accept(fd) == AcceptResult::Registered);
    }
    REQUIRE(reg.active_count() == kCap);

    // Offer many more; all are rejected and released.
    constexpr int kExcess = 50;
    int rejected = 0;
    for (int i = 0; i < kExcess; ++i) {
        const SocketHandle fd = 1000 + i;
        poller.os_open(fd);
        if (reg.accept(fd) == AcceptResult::RejectedAtCapacity) ++rejected;
        CHECK_FALSE(reg.is_registered(fd));
        CHECK_FALSE(poller.is_open(fd));       // released immediately
        CHECK_FALSE(poller.is_monitored(fd));  // never registered
    }

    CHECK(rejected == kExcess);
    CHECK(reg.active_count() == kCap);              // live set unchanged
    CHECK(poller.register_calls == kCap);           // only the first kCap
    CHECK(poller.close_calls == kExcess);           // one close per rejection
    CHECK(poller.clean());
}

TEST_CASE("lifecycle: a freed slot admits a new connection after teardown",
          "[integration][lifecycle][cap]") {
    // The cap counts LIVE connections, so tearing one down frees a slot for a
    // previously-rejected client end-to-end.
    constexpr std::uint32_t kCap = 2;
    InMemoryPoller poller;
    ConnectionRegistry reg(poller, kCap);

    for (SocketHandle fd : {1, 2}) {
        poller.os_open(fd);
        REQUIRE(reg.accept(fd) == AcceptResult::Registered);
    }
    const SocketHandle late = 3;
    poller.os_open(late);
    REQUIRE(reg.accept(late) == AcceptResult::RejectedAtCapacity);
    CHECK_FALSE(poller.is_open(late));

    // Free a slot, then the late client (re-opened by the OS) is admitted.
    reg.teardown(1, TeardownReason::PeerClosed);
    CHECK(reg.active_count() == 1);

    poller.os_open(late);
    CHECK(reg.accept(late) == AcceptResult::Registered);
    CHECK(reg.active_count() == 2);
    CHECK(poller.is_open(late));
    CHECK(poller.is_monitored(late));
    CHECK(poller.clean());
}

TEST_CASE("lifecycle: peer close releases resources well within 100 ms",
          "[integration][lifecycle][teardown]") {
    // When the client closes the connection, resources are released and
    // readiness monitoring stops within 100 ms. Teardown is a synchronous,
    // allocation-free path so it is effectively immediate; we measure it on a
    // steady clock and assert it lands far inside the bound.
    InMemoryPoller poller;
    ConnectionRegistry reg(poller);

    const SocketHandle fd = 77;
    poller.os_open(fd);
    REQUIRE(reg.accept(fd) == AcceptResult::Registered);
    REQUIRE(poller.is_open(fd));
    REQUIRE(poller.is_monitored(fd));

    const auto start = std::chrono::steady_clock::now();
    reg.teardown(fd, TeardownReason::PeerClosed);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    INFO("teardown took " << elapsed_ms << " ms");
    CHECK(elapsed < std::chrono::milliseconds(100));  // 100 ms bound

    // Resources fully released: not tracked, not monitored, not open.
    CHECK(reg.active_count() == 0);
    CHECK_FALSE(reg.is_registered(fd));
    CHECK_FALSE(poller.is_monitored(fd));  // stopped monitoring readiness
    CHECK_FALSE(poller.is_open(fd));       // socket closed
    CHECK(poller.deregister_calls == 1);
    CHECK(poller.close_calls == 1);
    CHECK(poller.clean());
}

TEST_CASE("lifecycle: a socket error closes via the same teardown path",
          "[integration][lifecycle][teardown]") {
    // A socket error condition closes the connection, releases its resources,
    // and stops readiness monitoring -- through the identical teardown path used
    // for peer close.
    InMemoryPoller poller;
    ConnectionRegistry reg(poller);

    const SocketHandle fd = 88;
    poller.os_open(fd);
    REQUIRE(reg.accept(fd) == AcceptResult::Registered);

    reg.teardown(fd, TeardownReason::SocketError);

    CHECK(reg.active_count() == 0);
    CHECK_FALSE(reg.is_registered(fd));
    CHECK_FALSE(poller.is_monitored(fd));
    CHECK_FALSE(poller.is_open(fd));
    CHECK(poller.deregister_calls == 1);
    CHECK(poller.close_calls == 1);
    CHECK(poller.clean());
}

TEST_CASE("lifecycle: peer close and socket error release identically",
          "[integration][lifecycle][teardown]") {
    // Peer close and socket error must produce the same observable resource
    // outcome -- both funnel through the single teardown path.
    auto final_state = [](TeardownReason reason) {
        InMemoryPoller poller;
        ConnectionRegistry reg(poller);
        const SocketHandle fd = 5;
        poller.os_open(fd);
        REQUIRE(reg.accept(fd) == AcceptResult::Registered);
        reg.teardown(fd, reason);
        return std::tuple{poller.register_calls, poller.deregister_calls,
                          poller.close_calls, poller.is_open(fd),
                          poller.is_monitored(fd)};
    };

    CHECK(final_state(TeardownReason::PeerClosed) ==
          final_state(TeardownReason::SocketError));
}

TEST_CASE("lifecycle: mixed end-to-end sequence ends with zero leaked resources",
          "[integration][lifecycle]") {
    // A realistic interleaving exercising every lifecycle branch in one run:
    // accept several connections, reject one past the cap, carry traffic, then
    // tear connections down via peer close and socket error. Afterwards no
    // socket may remain open or monitored.
    constexpr std::uint32_t kCap = 4;
    InMemoryPoller poller;
    ConnectionRegistry reg(poller, kCap);

    const SocketHandle a = 101, b = 102, c = 103, d = 104, e = 105;
    for (SocketHandle fd : {a, b, c, d}) {
        poller.os_open(fd);
        REQUIRE(reg.accept(fd) == AcceptResult::Registered);
    }

    // 5th connection exceeds the cap -> rejected + released.
    poller.os_open(e);
    REQUIRE(reg.accept(e) == AcceptResult::RejectedAtCapacity);
    CHECK_FALSE(poller.is_open(e));

    // Traffic flows on an open connection.
    std::vector<std::uint8_t> stream;
    append_encoded(stream, NewOrder{9, Side::Sell, 300, 5, 1});
    Sink sink;
    CHECK(reg.on_readable(a, std::span<const std::uint8_t>{stream}, sink));
    CHECK(sink.messages.size() == 1);

    // Tear down through both reasons.
    reg.teardown(a, TeardownReason::PeerClosed);
    reg.teardown(b, TeardownReason::SocketError);
    reg.teardown(c, TeardownReason::PeerClosed);
    reg.teardown(d, TeardownReason::SocketError);

    // Now a slot is free again and a re-opened client is admitted.
    poller.os_open(e);
    CHECK(reg.accept(e) == AcceptResult::Registered);
    reg.teardown(e, TeardownReason::PeerClosed);

    // Everything is released: no tracked connections, nothing open, nothing
    // monitored, and no resource-misuse flag tripped.
    CHECK(reg.active_count() == 0);
    CHECK(poller.open.empty());
    CHECK(poller.monitored.empty());
    CHECK(poller.clean());
}
