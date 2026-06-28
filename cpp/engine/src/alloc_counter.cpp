// Implementation of the hot-path dynamic-allocation counter.
//
// This translation unit does two things:
//   1. Defines the process-wide allocation counters and the AllocationCounter
//      reader API declared in hme/alloc_counter.hpp.
//   2. Replaces the global `operator new` / `operator delete` family so that
//      every dynamic allocation in the program increments those counters.
//
// Replacing the *global* allocation functions (rather than counting inside the
// engine) is what makes the counter trustworthy to an independent observer: it
// captures all heap traffic, including any that a future regression might
// accidentally introduce on the hot path. The observer needs no knowledge of
// engine internals -- it just reads the counters before and after a region.
//
// The counters are constant-initialised atomics so they are valid even for the
// very first allocation, which may happen during dynamic initialisation before
// main() runs. Increments use relaxed ordering: the hot path is single-threaded,
// and even across threads the counts only need to be eventually-accurate totals,
// not a synchronisation mechanism.

#include "hme/alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace hme::engine::detail {

// Process-wide counters. `constinit` guarantees constant initialisation, so
// these are zero before any dynamic initialiser (and any allocation) runs.
constinit std::atomic<std::uint64_t> g_allocations{0};
constinit std::atomic<std::uint64_t> g_deallocations{0};
constinit std::atomic<std::uint64_t> g_bytes_allocated{0};

// Record a single allocation of `size` bytes.
inline void note_allocation(std::size_t size) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    g_bytes_allocated.fetch_add(static_cast<std::uint64_t>(size),
                                std::memory_order_relaxed);
}

// Record a single deallocation. A null pointer is a no-op delete and is not
// counted (so allocation / deallocation counts stay paired for real blocks).
inline void note_deallocation(void* ptr) noexcept {
    if (ptr != nullptr) {
        g_deallocations.fetch_add(1, std::memory_order_relaxed);
    }
}

// Shared allocation routine for the throwing operator new overloads. Mirrors
// the standard behaviour: retry through the installed new-handler, and throw
// std::bad_alloc when no handler can free memory.
inline void* allocate_or_throw(std::size_t size) {
    // operator new(0) must return a unique, non-null pointer.
    const std::size_t request = (size == 0) ? 1 : size;
    while (true) {
        if (void* p = std::malloc(request)) {
            note_allocation(size);
            return p;
        }
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) {
            throw std::bad_alloc{};
        }
        handler();  // may free memory and return, or throw / terminate.
    }
}

// Shared allocation routine for the nothrow operator new overloads.
inline void* allocate_nothrow(std::size_t size) noexcept {
    try {
        return allocate_or_throw(size);
    } catch (...) {
        return nullptr;  // nothrow new reports failure as nullptr.
    }
}

}  // namespace hme::engine::detail

namespace hme::engine {

std::uint64_t AllocationCounter::allocation_count() noexcept {
    return detail::g_allocations.load(std::memory_order_relaxed);
}

std::uint64_t AllocationCounter::deallocation_count() noexcept {
    return detail::g_deallocations.load(std::memory_order_relaxed);
}

std::uint64_t AllocationCounter::bytes_allocated() noexcept {
    return detail::g_bytes_allocated.load(std::memory_order_relaxed);
}

AllocationCounter::Snapshot AllocationCounter::snapshot() noexcept {
    Snapshot s;
    s.allocations = detail::g_allocations.load(std::memory_order_relaxed);
    s.deallocations = detail::g_deallocations.load(std::memory_order_relaxed);
    s.bytes_allocated = detail::g_bytes_allocated.load(std::memory_order_relaxed);
    return s;
}

std::uint64_t AllocationCounter::allocations_since(const Snapshot& before) noexcept {
    return allocation_count() - before.allocations;
}

}  // namespace hme::engine

// ---------------------------------------------------------------------------
// Global allocation-function replacements.
//
// These are the replaceable, program-wide allocation functions ([new.delete]).
// Defining them here overrides the ones the standard library would otherwise
// provide, so every `new` / `delete` in the linked program flows through the
// counters above. We back the allocations with std::malloc / std::free, which
// honour the default new alignment on the supported platforms.
// ---------------------------------------------------------------------------

void* operator new(std::size_t size) {
    return hme::engine::detail::allocate_or_throw(size);
}

void* operator new[](std::size_t size) {
    return hme::engine::detail::allocate_or_throw(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return hme::engine::detail::allocate_nothrow(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return hme::engine::detail::allocate_nothrow(size);
}

void operator delete(void* ptr) noexcept {
    hme::engine::detail::note_deallocation(ptr);
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    hme::engine::detail::note_deallocation(ptr);
    std::free(ptr);
}

// Sized deletes ([new.delete.single] / [new.delete.array], C++14+). The size is
// only a hint to the allocator; std::free does not need it.
void operator delete(void* ptr, std::size_t) noexcept {
    hme::engine::detail::note_deallocation(ptr);
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    hme::engine::detail::note_deallocation(ptr);
    std::free(ptr);
}

// Nothrow deletes (paired with the nothrow news above).
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    hme::engine::detail::note_deallocation(ptr);
    std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    hme::engine::detail::note_deallocation(ptr);
    std::free(ptr);
}
