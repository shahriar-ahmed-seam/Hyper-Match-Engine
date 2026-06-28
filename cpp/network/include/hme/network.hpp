// Network_Server public surface.
//
// The raw TCP server uses OS-native readiness notification (epoll on Linux,
// IOCP on Windows) with per-connection framing.

#ifndef HME_NETWORK_HPP
#define HME_NETWORK_HPP

#include "hme/wire_protocol.hpp"

namespace hme::network {

// Maximum size of a single Binary_Message / per-connection framing buffer.
inline constexpr std::size_t kMaxMessageSize = 65536;

// Maximum number of concurrent client connections.
inline constexpr std::uint32_t kMaxConnections = 10000;

// Names the component so the `network` target is a real, buildable library.
const char* component_name() noexcept;

}  // namespace hme::network

#endif  // HME_NETWORK_HPP
