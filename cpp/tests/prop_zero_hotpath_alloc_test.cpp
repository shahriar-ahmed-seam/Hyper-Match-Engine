// Zero hot-path allocation. For any sequence of messages processed after the
// engine has entered its operational state, the exposed dynamic-allocation
// counter shows a delta of exactly zero from order ingress to order
// disposition.
//
// It asserts that the Matching_Engine's hot path performs ZERO dynamic
// allocations, measured by an independent observer (AllocationCounter) that
// brackets the hot region with a process-wide allocation snapshot.
//
// Methodology (bracket the hot path with a snapshot and assert the allocation
// delta is zero):
//
//   1. EVERYTHING that may legitimately allocate is done BEFORE the measured
//      region: the arbitrary batch of orders is generated (RapidCheck's `*`
//      dereference allocates), the EngineProcessor is constructed and brought
//      to its operational state, and a counter is pre-touched so no lazy
//      first-call allocation lands inside the window. Construction is allowed
//      to allocate; the hot path is not.
//
//   2. AllocationCounter::snapshot() is taken immediately BEFORE the hot region.
//
//   3. The hot region runs: submit() every pre-generated order onto the ingress
//      ring, process_all() to match/dispatch, and drain the egress ring with
//      next_event(). The harness deliberately avoids any allocating operation
//      inside this window -- it iterates the already-built input vector and
//      drains events into a single reused local plus scalar counters, never
//      growing a container.
//
//   4. allocations_since(before) is asserted to be exactly 0 after the region:
//      no dynamic allocation occurred from ingress to disposition.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p20` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

#include "hme/alloc_counter.hpp"
#include "hme/binary_message.hpp"
#include "hme/engine_processor.hpp"
#include "hme/wire_protocol.hpp"

using hme::BinaryMessage;
using hme::NewOrder;
using hme::Side;
using hme::engine::AllocationCounter;
using hme::engine::EngineProcessor;

namespace {

// Fixed reserved capacities for the processor under test. All storage (both
// rings and the book pools) is held inline and reserved at construction, so a
// constructed-and-initialized processor needs no further allocation on the hot
// path. The capacities are generous relative to the bounded batch below so the
// ingress ring never fills and the egress ring always holds the events a batch
// can produce (avoiding drops that would muddy the measurement).
constexpr std::size_t kMaxOrders_p20 = 512;
constexpr std::size_t kMaxPriceLevels_p20 = 512;
constexpr std::size_t kIngressCapacity_p20 = 512;
constexpr std::size_t kEgressCapacity_p20 = 2048;

using Processor_p20 =
    EngineProcessor<kMaxOrders_p20, kMaxPriceLevels_p20, kIngressCapacity_p20,
                    kEgressCapacity_p20>;

// Upper bound on the generated batch size, kept comfortably below the ingress
// capacity so the whole batch can be submitted before processing without the
// ingress ring filling.
constexpr std::size_t kMaxBatch_p20 = 200;

// A NewOrder specification produced by the generator. Ids, side, price, and
// quantity are chosen so the batch mixes resting, partially/ fully crossing,
// and (occasionally) invalid orders -- every dispatch path must be
// allocation-free, so spanning them strengthens the property.
struct OrderSpec_p20 {
    std::uint64_t order_id = 0;
    bool is_buy = true;
    std::uint64_t price_ticks = 0;
    std::uint32_t quantity = 0;
};

// Overlapping price band so generated orders frequently cross and trade.
constexpr std::uint64_t kMinPrice_p20 = 95;
constexpr std::uint64_t kMaxPrice_p20 = 115;  // inclusive
// Include 0 occasionally (an engine-invalid quantity) so the validation /
// rejection path is exercised too; it must also be allocation-free.
constexpr std::uint32_t kMaxQty_p20 = 50;

rc::Gen<OrderSpec_p20> gen_order_spec_p20() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange<std::uint64_t>(1, 1001),
                       rc::gen::arbitrary<bool>(),
                       rc::gen::inRange<std::uint64_t>(kMinPrice_p20,
                                                       kMaxPrice_p20 + 1),
                       rc::gen::inRange<std::uint32_t>(0, kMaxQty_p20 + 1)),
        [](const std::tuple<std::uint64_t, bool, std::uint64_t, std::uint32_t>&
               t) {
            return OrderSpec_p20{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                                 std::get<3>(t)};
        });
}

}  // namespace

TEST_CASE(
    "Property 20: processing an arbitrary batch through an operational engine "
    "performs zero hot-path dynamic allocations",
    "[loop][property][zero-alloc][hotpath]") {
    const bool ok = rc::check(
        "AllocationCounter delta around submit + process_all + drain is 0",
        [] {
            // ---- BEFORE the measured region: all permitted allocation -------

            // Generate the arbitrary batch (RapidCheck's `*` allocates) into a
            // vector built entirely outside the hot window.
            const std::vector<OrderSpec_p20> specs =
                *rc::gen::resize(
                    kMaxBatch_p20,
                    rc::gen::container<std::vector<OrderSpec_p20>>(
                        gen_order_spec_p20()));

            // Materialize the decoded inbound commands up front so the measured
            // region only iterates an already-built vector (no construction /
            // growth inside the window). reserve() pre-sizes the buffer.
            std::vector<NewOrder> orders;
            orders.reserve(specs.size());
            std::uint64_t seq = 1;
            for (const auto& s : specs) {
                NewOrder o;
                o.order_id = s.order_id;
                o.side = s.is_buy ? Side::Buy : Side::Sell;
                o.price_ticks = s.price_ticks;
                o.quantity = s.quantity;
                o.seq = seq++;
                orders.push_back(o);
            }

            // Construct the processor and bring it to its operational state.
            // Construction reserves all inline ring/book storage; this is the
            // one-time startup reservation and is explicitly allowed to
            // allocate -- it happens before the snapshot.
            auto proc = std::make_unique<Processor_p20>();
            RC_ASSERT(proc->initialize());
            RC_ASSERT(proc->operational());

            // Pre-touch the allocation counter so any lazy first-call cost is
            // paid before we start measuring.
            (void)AllocationCounter::snapshot();

            // A single reused local to drain egress events into -- declared
            // before the snapshot so its construction is not measured.
            BinaryMessage event;

            // ---- MEASURED HOT REGION: must allocate exactly zero ------------

            const AllocationCounter::Snapshot before =
                AllocationCounter::snapshot();

            // Order ingress: push every pre-built command onto the ingress ring.
            for (const NewOrder& o : orders) {
                proc->submit(o);  // try_push into pre-allocated ring; no alloc.
            }

            // Order processing: dispatch every queued message in FIFO order.
            const std::size_t processed = proc->process_all();

            // Order disposition: drain every emitted event from the egress ring
            // into the single reused local, counting rather than storing (no
            // growing container inside the window).
            std::uint64_t drained = 0;
            while (proc->next_event(event)) {
                ++drained;
            }

            const std::uint64_t allocations =
                AllocationCounter::allocations_since(before);

            // ---- AFTER the region: assertions (may allocate freely) ---------

            // Zero dynamic allocations across the whole ingress -> processing
            // -> disposition hot path.
            RC_ASSERT(allocations == 0);

            // Sanity: the batch really did flow through the hot path (otherwise
            // a no-op loop would trivially satisfy the zero-allocation claim).
            RC_ASSERT(processed == orders.size());
            (void)drained;
        });
    CHECK(ok);
}
