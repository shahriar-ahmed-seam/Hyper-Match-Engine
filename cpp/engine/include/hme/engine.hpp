// Matching_Engine public surface.
//
// The deterministic single-threaded, lock-free, zero-hot-path-allocation
// Matching_Engine runs on exactly one dedicated thread.

#ifndef HME_ENGINE_HPP
#define HME_ENGINE_HPP

#include "hme/wire_protocol.hpp"

namespace hme::engine {

// Names the component so the `engine` target links against the shared
// codec/Wire_Protocol header.
const char* component_name() noexcept;

}  // namespace hme::engine

#endif  // HME_ENGINE_HPP
