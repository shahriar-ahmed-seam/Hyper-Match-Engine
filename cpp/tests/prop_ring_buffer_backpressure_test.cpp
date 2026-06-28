// Ring-buffer-full back-pressure. For any ingress Ring_Buffer that is at full
// capacity, attempting to enqueue another Order is rejected with a
// back-pressure indication, leaves all previously buffered Orders unchanged, and
// performs zero dynamic memory allocation.
//
// Strategy. A generated sequence of try_push / try_pop operations is replayed
// against two things in lock-step:
//
//   * Subject: the fixed-capacity hme::engine::RingBuffer<int, Capacity> under
//     test.
//   * Reference model: a std::deque<int> capped at the same Capacity. A push is
//     accepted by the model only while the deque holds fewer than Capacity
//     elements (otherwise it is dropped, modelling back-pressure); a pop removes
//     the front element when non-empty.
//
// After every operation the test asserts the subject and the model agree on the
// boolean result of the operation, on the popped value (when a pop succeeds),
// and on size / empty / full. Because the model only ever accepts a push while
// below capacity, agreement witnesses three things at once: the ring never
// holds more than Capacity elements; once full, try_push returns false and does
// NOT drop, overwrite, or otherwise disturb the buffered contents (the rejected
// element is simply absent, never displacing an earlier one); and FIFO ordering
// is preserved (every popped value matches the model's front, i.e. elements pop
// in push order).
//
// The zero-dynamic-allocation half is verified directly: a ring is filled to
// capacity, then a window bracketed by the externally observable
// AllocationCounter runs only rejected try_push calls on the full ring; the
// observed allocation delta must be exactly zero, and a subsequent drain must
// return the originally buffered sentinels untouched and in FIFO order.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p21` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <tuple>
#include <vector>

#include "hme/alloc_counter.hpp"
#include "hme/ring_buffer.hpp"

using hme::engine::AllocationCounter;
using hme::engine::RingBuffer;

namespace {

// Small fixed capacity so a generated operation sequence repeatedly drives the
// ring to its full state (where the back-pressure contract is exercised) and
// also empties it (where try_pop must report empty). Element type is a trivial
// int whose own copy/move never allocates, so any allocation observed in the
// measured window could only come from the ring itself.
constexpr std::size_t kCapacity_p21 = 8;

using Ring_p21 = RingBuffer<int, kCapacity_p21>;

// Upper bound on the generated operation count. Comfortably larger than the
// capacity so the sequence overflows (and drains) the ring many times over.
constexpr std::size_t kMaxOps_p21 = 80;

// A single generated operation. `is_pop` selects try_pop; otherwise try_push of
// `value`. Push values come from a wide range so distinct elements are easy to
// track through the FIFO. The 1-in-3 `is_pop` roll keeps pushes in the majority
// so the ring spends plenty of time at full capacity under back-pressure.
struct Op_p21 {
    bool is_pop = false;
    int value = 0;
};

rc::Gen<Op_p21> gen_op_p21() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange<int>(0, 3),                 // kind roll
                       rc::gen::inRange<int>(-1000, 1000)),         // push value
        [](const std::tuple<int, int>& t) {
            return Op_p21{/*is_pop=*/std::get<0>(t) == 0,
                          /*value=*/std::get<1>(t)};
        });
}

}  // namespace

TEST_CASE(
    "Property 21: a full ring rejects pushes with back-pressure, preserves its "
    "contents in FIFO order, and allocates nothing",
    "[ring][property][backpressure]") {
    const bool ok = rc::check(
        "ring agrees with a capacity-capped deque model and never allocates on "
        "a full-ring push",
        [] {
            const std::vector<Op_p21> ops = *rc::gen::resize(
                kMaxOps_p21,
                rc::gen::container<std::vector<Op_p21>>(gen_op_p21()));

            // ---- Differential replay against a capacity-capped deque --------
            Ring_p21 ring;
            std::deque<int> model;  // reference; capped at kCapacity_p21.

            for (const Op_p21& op : ops) {
                if (op.is_pop) {
                    int out = 0x7FFFFFFF;  // sentinel; must be overwritten iff pop succeeds.
                    const bool ring_ok = ring.try_pop(out);
                    if (model.empty()) {
                        // Empty in the model -> the ring must also report empty
                        // and leave `out` untouched.
                        RC_ASSERT(!ring_ok);
                        RC_ASSERT(out == 0x7FFFFFFF);
                    } else {
                        // Non-empty -> FIFO front comes out of both.
                        RC_ASSERT(ring_ok);
                        RC_ASSERT(out == model.front());
                        model.pop_front();
                    }
                } else {
                    const bool ring_ok = ring.try_push(op.value);
                    if (model.size() < kCapacity_p21) {
                        // Space available -> accepted by both.
                        RC_ASSERT(ring_ok);
                        model.push_back(op.value);
                    } else {
                        // Full -> back-pressure: rejected by both, model (and
                        // therefore the buffered contents) unchanged.
                        RC_ASSERT(!ring_ok);
                    }
                }

                // After every operation the observable state must agree, and the
                // ring must never hold more than its fixed capacity.
                RC_ASSERT(ring.size() == model.size());
                RC_ASSERT(ring.size() <= kCapacity_p21);
                RC_ASSERT(ring.empty() == model.empty());
                RC_ASSERT(ring.full() == (model.size() == kCapacity_p21));
            }

            // Drain whatever remains and confirm it equals the model front-to-
            // back: the surviving elements are exactly the accepted pushes, in
            // push order, with every back-pressured element absent.
            while (!model.empty()) {
                int out = 0;
                RC_ASSERT(ring.try_pop(out));
                RC_ASSERT(out == model.front());
                model.pop_front();
            }
            RC_ASSERT(ring.empty());

            // ---- Zero-allocation back-pressure window -----------------------
            // Fill a fresh ring to capacity with known sentinels, then measure a
            // window that performs only rejected try_push calls on the full ring.
            Ring_p21 full_ring;
            for (std::size_t i = 0; i < kCapacity_p21; ++i) {
                RC_ASSERT(full_ring.try_push(static_cast<int>(i)));
            }
            RC_ASSERT(full_ring.full());

            // Pre-touch the counter so any lazy first-call cost is paid before
            // the measured region begins.
            (void)AllocationCounter::snapshot();

            // The measured region contains ONLY full-ring try_push calls and a
            // single reused scalar accumulator -- no RC_ASSERT, no container
            // growth -- so the only allocation it could observe would come from
            // the ring itself. (RC_ASSERT and friends are kept strictly outside
            // the window because their own bookkeeping may allocate.)
            bool all_rejected = true;
            const AllocationCounter::Snapshot before =
                AllocationCounter::snapshot();

            // Every push here hits a full ring: each must report back-pressure.
            for (int k = 0; k < 32; ++k) {
                all_rejected &= !full_ring.try_push(1'000'000 + k);
            }

            const std::uint64_t allocations =
                AllocationCounter::allocations_since(before);

            // Every full-ring push was rejected (back-pressure indication) ...
            RC_ASSERT(all_rejected);
            // ... and rejecting on a full ring performs zero dynamic memory
            // allocation.
            RC_ASSERT(allocations == 0);

            // The rejected pushes left the buffered sentinels untouched and in
            // FIFO order (nothing dropped or overwritten).
            RC_ASSERT(full_ring.size() == kCapacity_p21);
            for (std::size_t i = 0; i < kCapacity_p21; ++i) {
                int out = -1;
                RC_ASSERT(full_ring.try_pop(out));
                RC_ASSERT(out == static_cast<int>(i));
            }
            RC_ASSERT(full_ring.empty());
        });
    CHECK(ok);
}
