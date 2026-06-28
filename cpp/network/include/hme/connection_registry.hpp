// Connection lifecycle management for the Network_Server event loop.
//
// The server is a raw TCP server that uses OS-native readiness notification
// (epoll on Linux, IOCP on Windows). The OS polling itself is platform-specific
// and awkward to unit-test, so the server splits into two pieces:
//
//   1. ReadinessPoller -- a thin, OS-specific adapter that owns the actual
//      epoll/IOCP descriptor, registers/deregisters sockets for readiness, and
//      closes sockets. This is the only part that touches platform syscalls.
//
//   2. ConnectionRegistry -- the OS-agnostic connection-lifecycle state machine
//      that this header implements. It enforces the 10,000-connection cap,
//      keeps per-connection framing state (FrameBuffer), and funnels every
//      connection close (oversize frame, peer close, socket error) through ONE
//      teardown path. Because it depends only on the ReadinessPoller interface,
//      its accept/cap/teardown logic is fully deterministic and testable with a
//      fake poller.
//
// The event loop (NetworkServer::run, the OS adapter) is therefore reduced to:
// accept a socket -> registry.accept(); a socket is readable -> registry
// .on_readable(); a socket reports hangup/error -> registry.teardown(). The
// interesting lifecycle decisions all live here, behind the interface.

#ifndef HME_CONNECTION_REGISTRY_HPP
#define HME_CONNECTION_REGISTRY_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>

#include "hme/frame_buffer.hpp"
#include "hme/network.hpp"

namespace hme::network {

// A client socket handle. Mirrors the design's `int fd`; the OS adapter maps it
// to the native handle type (an `int` epoll fd on Linux, a SOCKET on Windows).
using SocketHandle = int;

// Outcome of offering a freshly-accepted client socket to the registry.
enum class AcceptResult : std::uint8_t {
    // Accepted: the socket was registered for OS-native readiness notification
    // and per-connection framing state was created.
    Registered = 0,
    // Rejected: accepting would exceed the concurrent-connection cap, so the
    // socket was closed and any resources allocated for it were released
    // WITHOUT being registered.
    RejectedAtCapacity = 1,
};

// Why a connection was torn down. Every reason funnels through the single
// teardown path; the reason is informational (e.g. logging).
enum class TeardownReason : std::uint8_t {
    OversizeFrame = 0,  // undecodable/oversize frame from the framer
    PeerClosed = 1,     // the client closed the TCP connection
    SocketError = 2,    // the socket reported an error condition
};

// OS-native readiness adapter contract. The epoll (Linux) / IOCP (Windows)
// implementation is a thin adapter over this interface; ConnectionRegistry
// drives it but knows nothing about the underlying mechanism, which keeps the
// lifecycle logic portable and testable.
class ReadinessPoller {
public:
    virtual ~ReadinessPoller() = default;

    // Begin monitoring `fd` for OS-native readiness notification.
    virtual void register_readiness(SocketHandle fd) = 0;

    // Stop monitoring `fd`'s readiness events. Part of the single teardown path.
    virtual void deregister_readiness(SocketHandle fd) = 0;

    // Close the underlying socket and release its OS-level resources.
    virtual void close_socket(SocketHandle fd) = 0;
};

// The OS-agnostic connection-lifecycle state machine.
//
// Not thread-safe by design: the Network_Server runs a single event-loop
// thread (consistent with the engine's single-threaded model). No method
// allocates beyond the per-connection state created on accept and released on
// teardown; the per-message framing path (on_readable) is allocation-free.
class ConnectionRegistry {
public:
    // `poller` must outlive this registry. `max_connections` is the concurrent
    // connection cap (defaults to the limit of 10,000); it is injectable so
    // tests can exercise the cap without opening 10,000 sockets.
    explicit ConnectionRegistry(
        ReadinessPoller& poller,
        std::uint32_t max_connections = kMaxConnections) noexcept
        : poller_(poller), max_connections_(max_connections) {}

    // Offer a freshly-accepted client socket `fd` to the registry.
    //
    // When below the cap, register `fd` for readiness and create its
    // per-connection framing state -> AcceptResult::Registered.
    //
    // When accepting would exceed the cap, close `fd` and release any resources
    // allocated for it WITHOUT registering -> RejectedAtCapacity. The set of
    // tracked connections is left unchanged.
    AcceptResult accept(SocketHandle fd) {
        // Reject when already at the cap: accepting another would exceed it.
        if (connections_.size() >= max_connections_) {
            poller_.close_socket(fd);  // release the resource allocated for it
            return AcceptResult::RejectedAtCapacity;
        }
        // Track per-connection framing state, then register for readiness.
        connections_.try_emplace(fd);
        poller_.register_readiness(fd);
        return AcceptResult::Registered;
    }

    // A registered socket reported readable bytes: feed `bytes` to its framer,
    // delivering each complete Binary_Message to `deliver` in arrival order.
    // `deliver` is any callable invocable with `const BinaryMessage&`.
    //
    // Returns true if the connection remains open afterwards. If the framer
    // signals closure (an oversize or undecodable frame) the connection is torn
    // down through the single teardown path and false is returned. An unknown
    // `fd` (already torn down) is a no-op returning false.
    template <typename Deliver>
    bool on_readable(SocketHandle fd, std::span<const std::uint8_t> bytes,
                     Deliver&& deliver) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return false;  // not tracked (already torn down) -> nothing to do
        }
        const FrameStatus status =
            it->second.framer.consume(bytes, std::forward<Deliver>(deliver));
        if (status == FrameStatus::Closed) {
            // Oversize/undecodable frame -> the single teardown path.
            teardown(fd, TeardownReason::OversizeFrame);
            return false;
        }
        return true;
    }

    // The single teardown path. Stops monitoring the socket's readiness events,
    // closes the socket, and releases its per-connection resources. Oversize
    // frames, peer close, and socket errors all route here so teardown happens
    // exactly one way.
    //
    // Idempotent: tearing down an unknown/already-released `fd` is a no-op, so
    // overlapping events (e.g. error + hangup on the same socket) are safe.
    void teardown(SocketHandle fd, TeardownReason /*reason*/) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;  // already released; nothing further to do
        }
        // Deregister readiness first so no further events are dispatched for
        // this socket, then close it, then release per-connection state.
        poller_.deregister_readiness(fd);
        poller_.close_socket(fd);
        connections_.erase(it);
    }

    // Number of currently tracked (open, registered) connections.
    std::uint32_t active_count() const noexcept {
        return static_cast<std::uint32_t>(connections_.size());
    }

    // The configured concurrent-connection cap.
    std::uint32_t max_connections() const noexcept { return max_connections_; }

    // True if `fd` is currently tracked/registered.
    bool is_registered(SocketHandle fd) const noexcept {
        return connections_.find(fd) != connections_.end();
    }

    // Retained partial-frame byte count for `fd`, or 0 if `fd` is not tracked.
    // Exposed for tests/observability; never exceeds kMaxMessageSize.
    std::size_t buffered(SocketHandle fd) const noexcept {
        auto it = connections_.find(fd);
        return it == connections_.end() ? 0 : it->second.framer.buffered();
    }

private:
    // Per-connection state: the framing buffer that reassembles whole
    // Binary_Messages from arbitrary socket reads.
    struct Connection {
        FrameBuffer framer;
    };

    ReadinessPoller& poller_;
    std::uint32_t max_connections_;
    std::unordered_map<SocketHandle, Connection> connections_;
};

}  // namespace hme::network

#endif  // HME_CONNECTION_REGISTRY_HPP
