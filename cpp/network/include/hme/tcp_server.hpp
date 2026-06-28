// Real, runnable TCP server for the Hyper-Match-Engine.
//
// This is the OS-facing event loop kept out of ConnectionRegistry /
// ServerPipeline (see connection_registry.hpp): those pieces own the portable,
// testable lifecycle + framing state machine, while this file owns the platform
// syscalls (socket / bind / listen / accept / recv / send / select) behind the
// ReadinessPoller contract.
//
// Responsibilities, and just as importantly NON-responsibilities:
//   * It MOVES BYTES ONLY. It never parses the Wire_Protocol. Inbound bytes are
//     handed verbatim to ServerPipeline::submit_client_bytes (which frames and
//     decodes them); outbound response frames produced by the pipeline are
//     handed back through the out-sink and queued verbatim for the socket.
//   * It owns the listening socket, a SelectReadinessPoller (the set of
//     monitored connection fds), and one outbound byte queue per connection.
//   * Connection lifecycle decisions (cap, teardown) are delegated to the
//     pipeline / registry; this loop only reacts to readiness and EOF/errors.
//
// Portability: everything platform-specific is guarded by _WIN32. On Windows it
// uses Winsock2 (and links ws2_32); on POSIX it uses the BSD socket API. The
// readiness mechanism here is select() with a short timeout so the loop can
// poll an atomic stop flag -- simple and sufficient for a single-threaded
// engine front-end. (epoll/IOCP would be the production swap, behind the same
// ReadinessPoller contract.)

#ifndef HME_TCP_SERVER_HPP
#define HME_TCP_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hme/connection_registry.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hme::network {

// Native socket handle type. SocketHandle (the portable contract type) is
// `int`; on Windows the real OS handle is a SOCKET (an unsigned pointer-sized
// value), so we convert at the syscall boundary. For an engine front-end the
// small set of live fds fits comfortably in an int.
#ifdef _WIN32
using NativeSocket = SOCKET;
#else
using NativeSocket = int;
#endif

// RAII guard for Winsock initialization. Constructed once before any socket
// call and torn down on scope exit. A no-op on POSIX so call sites stay clean.
class WinsockGuard {
public:
    WinsockGuard();
    ~WinsockGuard();
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
};

namespace detail {

// Convert the portable handle to the native one used by syscalls.
inline NativeSocket to_native(SocketHandle fd) noexcept {
    return static_cast<NativeSocket>(fd);
}

// Put a socket into non-blocking mode. Returns true on success.
bool set_nonblocking(SocketHandle fd) noexcept;

// Close a native socket (closesocket on Windows, ::close on POSIX).
void close_native(SocketHandle fd) noexcept;

// Last socket error code for the calling thread.
int last_socket_error() noexcept;

// True when `err` is the "operation would block" / "try again" condition.
bool would_block(int err) noexcept;

}  // namespace detail

// A ReadinessPoller backed by a plain fd set, designed to drive a select()
// loop. It records which sockets are being monitored (register/deregister) and
// performs the actual socket close (close_socket), exactly the three operations
// ConnectionRegistry funnels through the single teardown path.
//
// The server loop reads `monitored()` each iteration to build its read fd_set.
class SelectReadinessPoller : public ReadinessPoller {
public:
    void register_readiness(SocketHandle fd) override;
    void deregister_readiness(SocketHandle fd) override;
    void close_socket(SocketHandle fd) override;

    // The current set of monitored connection sockets (the server builds its
    // read fd_set from this each loop iteration).
    const std::unordered_set<SocketHandle>& monitored() const noexcept {
        return monitored_;
    }

    // Number of close_socket() calls performed (observability).
    std::uint64_t closed_count() const noexcept { return closed_count_; }

private:
    std::unordered_set<SocketHandle> monitored_;
    std::uint64_t closed_count_ = 0;
};

// The select()-driven TCP event loop. Templated on the pipeline type so it can
// drive any ServerPipeline<...> instantiation without erasing its capacities.
//
// `Pipeline` must expose: accept(SocketHandle) -> AcceptResult,
// teardown(SocketHandle, TeardownReason), and
// submit_client_bytes(SocketHandle, std::span<const std::uint8_t>, OutSink)
// -> FrameStatus (exactly ServerPipeline's surface).
template <class Pipeline>
class TcpServer {
public:
    // Bind+listen immediately so failures surface at construction. `poller`
    // must be the same SelectReadinessPoller the `pipeline` was built over, so
    // accepts/teardowns and this loop share one monitored fd set. Throws
    // std::runtime_error if the socket cannot be created/bound/listened.
    TcpServer(const std::string& host, std::uint16_t port,
              Pipeline& pipeline, SelectReadinessPoller& poller)
        : pipeline_(pipeline), poller_(poller), host_(host), port_(port) {
        open_listen_socket();
    }

    ~TcpServer() {
        if (listen_fd_ != kInvalid) {
            detail::close_native(listen_fd_);
        }
    }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Request the loop stop. Safe to call from a signal handler / other thread:
    // the loop polls this flag every select() timeout (<= 200 ms).
    void stop() noexcept { stop_.store(true, std::memory_order_relaxed); }

    // The configured listen port (useful when port 0 was requested, though this
    // server takes an explicit port).
    std::uint16_t port() const noexcept { return port_; }

    // Observability.
    std::uint64_t accepted_count() const noexcept { return accepted_; }
    std::uint64_t rejected_count() const noexcept { return rejected_; }
    std::uint64_t closed_count() const noexcept { return closed_; }

    // Run the event loop until stop() is requested. Single-threaded.
    void run() {
        stop_.store(false, std::memory_order_relaxed);
        while (!stop_.load(std::memory_order_relaxed)) {
            fd_set read_set;
            fd_set write_set;
            FD_ZERO(&read_set);
            FD_ZERO(&write_set);

            // Always watch the listen socket for new connections.
            FD_SET(detail::to_native(listen_fd_), &read_set);
            NativeSocket max_fd = detail::to_native(listen_fd_);

            // Watch every monitored connection for readability.
            for (SocketHandle fd : poller_.monitored()) {
                const NativeSocket nfd = detail::to_native(fd);
                FD_SET(nfd, &read_set);
                if (nfd > max_fd) {
                    max_fd = nfd;
                }
            }

            // Watch connections with queued outbound bytes for writability.
            for (const auto& [fd, buf] : outbound_) {
                if (!buf.empty()) {
                    const NativeSocket nfd = detail::to_native(fd);
                    FD_SET(nfd, &write_set);
                    if (nfd > max_fd) {
                        max_fd = nfd;
                    }
                }
            }

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 200 * 1000;  // 200 ms -> re-check stop flag.

#ifdef _WIN32
            // Windows ignores the nfds argument.
            const int ready = ::select(0, &read_set, &write_set, nullptr, &timeout);
#else
            const int ready =
                ::select(static_cast<int>(max_fd) + 1, &read_set, &write_set,
                         nullptr, &timeout);
#endif
            if (ready < 0) {
                if (detail::would_block(detail::last_socket_error())) {
                    continue;  // interrupted/transient -> just loop again
                }
                continue;
            }
            if (ready == 0) {
                continue;  // timeout: poll stop flag and rebuild sets
            }

            // New connections first.
            if (FD_ISSET(detail::to_native(listen_fd_), &read_set)) {
                drain_accept();
            }

            // Readable connections. Snapshot the monitored set because handling
            // a read can tear the connection down (mutating the set).
            read_scratch_.assign(poller_.monitored().begin(),
                                  poller_.monitored().end());
            for (SocketHandle fd : read_scratch_) {
                if (FD_ISSET(detail::to_native(fd), &read_set)) {
                    handle_readable(fd);
                }
            }

            // Writable connections. Snapshot the keys for the same reason.
            write_scratch_.clear();
            for (const auto& [fd, buf] : outbound_) {
                if (!buf.empty() &&
                    FD_ISSET(detail::to_native(fd), &write_set)) {
                    write_scratch_.push_back(fd);
                }
            }
            for (SocketHandle fd : write_scratch_) {
                handle_writable(fd);
            }
        }
    }

private:
    static constexpr SocketHandle kInvalid = static_cast<SocketHandle>(-1);
    // A stack read buffer; one socket read is drained per readable event.
    static constexpr std::size_t kReadBufferSize = 64 * 1024;

    void open_listen_socket() {
        const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
        if (s == INVALID_SOCKET) {
            throw std::runtime_error("socket() failed");
        }
#else
        if (s < 0) {
            throw std::runtime_error("socket() failed");
        }
#endif
        listen_fd_ = static_cast<SocketHandle>(s);

        // Allow quick restart on the same port.
        int reuse = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        if (host_.empty() || host_ == "0.0.0.0") {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
            detail::close_native(listen_fd_);
            listen_fd_ = kInvalid;
            throw std::runtime_error("invalid host address: " + host_);
        }

        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            detail::close_native(listen_fd_);
            listen_fd_ = kInvalid;
            throw std::runtime_error("bind() failed on port " +
                                     std::to_string(port_));
        }
        if (::listen(s, SOMAXCONN) != 0) {
            detail::close_native(listen_fd_);
            listen_fd_ = kInvalid;
            throw std::runtime_error("listen() failed");
        }
        if (!detail::set_nonblocking(listen_fd_)) {
            detail::close_native(listen_fd_);
            listen_fd_ = kInvalid;
            throw std::runtime_error("failed to set listen socket non-blocking");
        }
    }

    // Accept every pending connection (the listen socket is non-blocking, so we
    // loop until accept() reports it would block).
    void drain_accept() {
        for (;;) {
            const NativeSocket client = ::accept(detail::to_native(listen_fd_),
                                                 nullptr, nullptr);
#ifdef _WIN32
            if (client == INVALID_SOCKET) {
                break;  // no more pending connections (or transient error)
            }
#else
            if (client < 0) {
                break;
            }
#endif
            const SocketHandle fd = static_cast<SocketHandle>(client);
            // Best-effort non-blocking; if it fails, close and skip.
            if (!detail::set_nonblocking(fd)) {
                detail::close_native(fd);
                continue;
            }
            // Hand to the pipeline: it registers (via the poller) or rejects +
            // closes at capacity. We never touch protocol bytes here.
            const AcceptResult result = pipeline_.accept(fd);
            if (result == AcceptResult::Registered) {
                ++accepted_;
                outbound_.try_emplace(fd);  // empty outbound queue for it
            } else {
                ++rejected_;  // pipeline already closed it at capacity
            }
        }
    }

    // A connection is readable: drain one recv() and push the bytes through the
    // pipeline. Response frames are appended to the connection's outbound queue.
    void handle_readable(SocketHandle fd) {
        std::uint8_t buffer[kReadBufferSize];
        const auto n = ::recv(detail::to_native(fd),
                              reinterpret_cast<char*>(buffer),
                              static_cast<int>(sizeof(buffer)), 0);
        if (n == 0) {
            // Peer closed cleanly.
            pipeline_.teardown(fd, TeardownReason::PeerClosed);
            drop_connection(fd);
            return;
        }
        if (n < 0) {
            if (detail::would_block(detail::last_socket_error())) {
                return;  // spurious readiness; nothing to read yet
            }
            pipeline_.teardown(fd, TeardownReason::SocketError);
            drop_connection(fd);
            return;
        }

        const FrameStatus status = pipeline_.submit_client_bytes(
            fd, std::span<const std::uint8_t>{buffer, static_cast<std::size_t>(n)},
            [this](SocketHandle out_fd, std::span<const std::uint8_t> frame) {
                auto& queue = outbound_[out_fd];
                queue.insert(queue.end(), frame.begin(), frame.end());
            });

        if (status == FrameStatus::Closed) {
            // The pipeline tore the connection down (oversize/undecodable frame
            // or non-operational engine). Drop our outbound buffer for it.
            drop_connection(fd);
            return;
        }

        // Opportunistically flush any response bytes we just queued.
        auto it = outbound_.find(fd);
        if (it != outbound_.end() && !it->second.empty()) {
            handle_writable(fd);
        }
    }

    // A connection is writable: send as much of its outbound queue as possible.
    void handle_writable(SocketHandle fd) {
        auto it = outbound_.find(fd);
        if (it == outbound_.end() || it->second.empty()) {
            return;
        }
        std::vector<std::uint8_t>& queue = it->second;
        std::size_t sent_total = 0;
        while (sent_total < queue.size()) {
            const auto n = ::send(
                detail::to_native(fd),
                reinterpret_cast<const char*>(queue.data() + sent_total),
                static_cast<int>(queue.size() - sent_total), 0);
            if (n > 0) {
                sent_total += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && detail::would_block(detail::last_socket_error())) {
                break;  // kernel buffer full -> keep the remainder queued
            }
            // Real send error -> tear the connection down.
            pipeline_.teardown(fd, TeardownReason::SocketError);
            drop_connection(fd);
            return;
        }
        // Erase the sent prefix; keep any unsent remainder for the next pass.
        if (sent_total > 0) {
            queue.erase(queue.begin(),
                        queue.begin() + static_cast<std::ptrdiff_t>(sent_total));
        }
    }

    // Remove a connection's server-side outbound state. The socket itself is
    // closed by the registry/poller through the single teardown path.
    void drop_connection(SocketHandle fd) {
        outbound_.erase(fd);
        ++closed_;
    }

    Pipeline& pipeline_;
    SelectReadinessPoller& poller_;
    std::string host_;
    std::uint16_t port_;
    SocketHandle listen_fd_ = kInvalid;
    std::atomic<bool> stop_{false};

    // Per-connection outbound byte queues. This is connection state OUTSIDE the
    // hot matching path, so allocation here is acceptable (the only data-path
    // allocation is per-connection accept/teardown state).
    std::unordered_map<SocketHandle, std::vector<std::uint8_t>> outbound_;

    // Reused scratch vectors so the loop body itself does not allocate per pass.
    std::vector<SocketHandle> read_scratch_;
    std::vector<SocketHandle> write_scratch_;

    std::uint64_t accepted_ = 0;
    std::uint64_t rejected_ = 0;
    std::uint64_t closed_ = 0;
};

}  // namespace hme::network

#endif  // HME_TCP_SERVER_HPP
