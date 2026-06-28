// Platform syscall implementations for the TCP server (see tcp_server.hpp).
//
// Only the non-templated, platform-specific pieces live here: the Winsock RAII
// guard, the small socket helpers (non-blocking / close / error), and the
// SelectReadinessPoller's three operations. The select() event loop itself is
// templated on the pipeline type and therefore lives in the header.

#include "hme/tcp_server.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace hme::network {

// ---- WinsockGuard ----------------------------------------------------------

WinsockGuard::WinsockGuard() {
#ifdef _WIN32
    WSADATA wsa_data;
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (rc != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif
}

WinsockGuard::~WinsockGuard() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// ---- Socket helpers --------------------------------------------------------

namespace detail {

bool set_nonblocking(SocketHandle fd) noexcept {
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(to_native(fd), FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void close_native(SocketHandle fd) noexcept {
#ifdef _WIN32
    ::closesocket(to_native(fd));
#else
    ::close(fd);
#endif
}

int last_socket_error() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool would_block(int err) noexcept {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EWOULDBLOCK || err == EAGAIN || err == EINTR;
#endif
}

}  // namespace detail

// ---- SelectReadinessPoller -------------------------------------------------

void SelectReadinessPoller::register_readiness(SocketHandle fd) {
    monitored_.insert(fd);
}

void SelectReadinessPoller::deregister_readiness(SocketHandle fd) {
    monitored_.erase(fd);
}

void SelectReadinessPoller::close_socket(SocketHandle fd) {
    detail::close_native(fd);
    ++closed_count_;
}

}  // namespace hme::network
