// Smoke / unit tests for the hot-path dynamic-allocation counter.
//
// The dynamic-allocation counter is exposed so that an independent observer can
// verify zero-allocation behavior without inspecting engine internals. These
// tests act as that independent observer: they only touch the public
// hme::engine::AllocationCounter API and never reach into any engine data
// structure.
//
// The numbered correctness property for the engine actually keeping the counter
// at zero across the hot path is validated separately; here we verify the
// *mechanism* -- that the counter exists, is readable, counts real allocations,
// and shows a zero delta around allocation-free code.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

#include "hme/alloc_counter.hpp"

using hme::engine::AllocationCounter;

namespace {

// Force a heap allocation to actually be performed under optimisation.
//
// Since C++14 ([expr.new]/N3664) the compiler is allowed to elide and coalesce
// calls to the replaceable global operator new/delete -- even when those have
// been replaced with observable side effects, as they are here. At -O2/-O3 GCC
// readily proves that a `new`/`delete` pair around a value that never escapes
// is unnecessary, converts the object to a stack temporary, and removes the
// allocation entirely (driving the counter delta to 0).
//
// Reading only the pointed-to *value* (e.g. `volatile int v = *p;`) does not
// prevent this: the optimiser keeps the value on the stack and still drops the
// allocation. To keep the allocation observable we must make the *pointer*
// escape to code the optimiser cannot see through, so it can no longer prove
// the allocation is dead. This does not weaken what the test verifies -- the
// allocation is still a genuine heap allocation routed through operator new.
template <typename T>
inline void keep_alive(T* ptr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    // The pointer escapes into an empty asm block with a memory clobber, so the
    // compiler must materialise the allocation and cannot reorder/remove it.
    asm volatile("" : : "g"(ptr) : "memory");
#else
    // Portable fallback: storing the pointer through a volatile sink is an
    // observable side effect that consumes the operator new result.
    static void* volatile sink;
    sink = static_cast<void*>(const_cast<typename std::remove_const<T>::type*>(ptr));
#endif
}

}  // namespace

TEST_CASE("Allocation counter is externally readable", "[alloc][req7.3]") {
    // An observer can sample every counter through the public API alone.
    const AllocationCounter::Snapshot s = AllocationCounter::snapshot();
    CHECK(AllocationCounter::allocation_count() >= s.allocations);
    CHECK(AllocationCounter::deallocation_count() >= s.deallocations);
    CHECK(AllocationCounter::bytes_allocated() >= s.bytes_allocated);
}

TEST_CASE("Allocation counter observes a real dynamic allocation", "[alloc][req7.3]") {
    const AllocationCounter::Snapshot before = AllocationCounter::snapshot();

    // A heap allocation the observer did not perform through engine internals.
    // Escaping the pointer via keep_alive() stops the optimiser from proving the
    // allocation is dead and eliding it under -O2/-O3, so the counter observes a
    // real operator new call.
    int* p = new int(7);
    keep_alive(p);
    CHECK(*p == 7);

    const std::uint64_t allocs = AllocationCounter::allocations_since(before);
    CHECK(allocs >= 1);
    CHECK(AllocationCounter::bytes_allocated() >= before.bytes_allocated + sizeof(int));

    const std::uint64_t deallocs_before = AllocationCounter::deallocation_count();
    delete p;
    CHECK(AllocationCounter::deallocation_count() == deallocs_before + 1);
}

TEST_CASE("Allocation counter shows zero delta across allocation-free code",
          "[alloc][req7.3]") {
    // Pre-touch a stack buffer so nothing in the measured region allocates.
    int scratch[64] = {};

    const AllocationCounter::Snapshot before = AllocationCounter::snapshot();

    // Allocation-free work: pure integer arithmetic over a stack array. This
    // models the hot-path contract at the mechanism level -- no `new`, no
    // growing containers.
    std::uint64_t acc = 0;
    for (int i = 0; i < 64; ++i) {
        scratch[i] = i * 3;
        acc += static_cast<std::uint64_t>(scratch[i]);
    }
    volatile std::uint64_t sink = acc;
    (void)sink;

    CHECK(AllocationCounter::allocations_since(before) == 0);
    CHECK(AllocationCounter::deallocation_count() == before.deallocations);
}

TEST_CASE("Allocation counter pairs array new/delete", "[alloc][req7.3]") {
    const AllocationCounter::Snapshot before = AllocationCounter::snapshot();

    {
        auto buf = std::make_unique<int[]>(128);
        buf[0] = 1;
        buf[127] = 2;
        // Escape the buffer pointer so the array allocation cannot be elided.
        keep_alive(buf.get());
        CHECK(buf[0] + buf[127] == 3);
        // unique_ptr frees on scope exit -> one matching deallocation.
    }

    CHECK(AllocationCounter::allocations_since(before) >= 1);
    CHECK(AllocationCounter::deallocation_count() >= before.deallocations + 1);
}
