// Externally observable hot-path dynamic-allocation counter.
//
// The Matching_Engine performs zero dynamic memory allocations on the hot path.
// To let an independent observer verify that without inspecting engine
// internals, this component instruments the program's global allocation
// functions (operator new / operator delete) and exposes the running counts
// through a small read-only API.
//
// Why instrument global operator new?
//   * It is the single choke point through which every C++ dynamic allocation
//     flows (raw `new`, `std::vector` growth, `std::string`, etc.), so the
//     counter observes all heap activity regardless of which code triggers it.
//     An observer brackets the hot path with a snapshot and asserts the
//     allocation delta is zero.
//   * The counts are process-wide and externally readable.
//
// Linking note: the global operator new/delete replacements live in the same
// translation unit as the reader functions below (alloc_counter.cpp). Because
// any verifier calls one of these reader functions, the linker is forced to
// pull that object file in from the static `engine` library, which in turn
// installs the global allocation-counting operators for the whole program.

#ifndef HME_ALLOC_COUNTER_HPP
#define HME_ALLOC_COUNTER_HPP

#include <cstddef>
#include <cstdint>

namespace hme::engine {

// Read-only observer over the process-wide dynamic-allocation counters.
//
// All methods are static and cheap (a single relaxed atomic load each): an
// observer can sample them immediately before and after a region of hot-path
// code and compare the deltas. The counters are monotonically non-decreasing
// for the lifetime of the process.
class AllocationCounter {
public:
    // A point-in-time sample of every counter, taken atomically per field.
    // Intended to bracket a region of code so the caller can measure how many
    // allocations / deallocations / bytes occurred within it.
    struct Snapshot {
        std::uint64_t allocations = 0;    // operator new calls so far.
        std::uint64_t deallocations = 0;  // operator delete calls so far.
        std::uint64_t bytes_allocated = 0;  // total bytes requested via new.
    };

    // Total number of dynamic allocations (operator new invocations) observed
    // process-wide since startup. In the engine's steady operational state this
    // value does not change across the hot path.
    static std::uint64_t allocation_count() noexcept;

    // Total number of dynamic deallocations (operator delete invocations).
    static std::uint64_t deallocation_count() noexcept;

    // Total number of bytes requested through operator new since startup.
    static std::uint64_t bytes_allocated() noexcept;

    // Capture all counters in one call. Each field is read with a relaxed
    // atomic load; the snapshot is "consistent enough" for delta measurement
    // around single-threaded hot-path code.
    static Snapshot snapshot() noexcept;

    // Convenience: number of allocations that have occurred since `before` was
    // captured. Equivalent to `allocation_count() - before.allocations`.
    static std::uint64_t allocations_since(const Snapshot& before) noexcept;
};

}  // namespace hme::engine

#endif  // HME_ALLOC_COUNTER_HPP
