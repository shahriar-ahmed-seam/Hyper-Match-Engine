// FIFO processing order. For any sequence of messages enqueued on the ingress
// Ring_Buffer, the Matching_Engine processes them in the exact first-in-
// first-out order in which they were enqueued, with no reordering or skipping.
//
// Strategy. A generated sequence of inbound commands (NewOrder / CancelOrder,
// including occasional invalid orders) is run through the EngineProcessor two
// independent ways and the emitted egress event sequences are compared:
//
//   * Reference (unambiguously arrival order): submit ONE command, immediately
//     process_next(), drain its events, then repeat for the next command. By
//     construction the reference can only ever process the commands in the
//     exact order they were enqueued.
//
//   * Subject (the loop under test): submit the WHOLE sequence onto the ingress
//     ring first, then process_all() and drain every emitted event.
//
// Because each command's emitted events depend on the Order_Book state left by
// every command before it, any reordering or skipping by the ingress ring /
// process loop would make the subject's event sequence diverge from the strict
// arrival-order reference. Equality of the two event sequences (and a processed
// count equal to the number of enqueued messages) therefore witnesses strict
// FIFO processing with no reordering or skipping.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p17` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstdint>
#include <tuple>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/engine_processor.hpp"
#include "hme/wire_protocol.hpp"

using hme::BinaryMessage;
using hme::CancelOrder;
using hme::NewOrder;
using hme::Side;
using hme::engine::EngineProcessor;

namespace {

// Ample fixed capacities: a resting-order/price-level pool and an ingress ring
// large enough to hold a whole generated batch before any processing, plus an
// egress ring large enough to hold every event the batch can emit in one
// process_all() drain (the subject never drains mid-run).
using Processor_p17 = EngineProcessor<256, 256, 256, 4096>;

// Upper bound on the generated command count, kept well below the ingress
// capacity so every submit() in the subject succeeds before processing begins.
constexpr std::size_t kMaxCommands_p17 = 60;

// A single generated inbound command. `is_cancel` selects CancelOrder (carrying
// only order_id); otherwise a NewOrder with the remaining fields. Domains are
// small and overlapping so commands frequently cross (producing Trades), rest,
// and cancels frequently hit a resting id (producing Acks) or miss (producing
// Rejects) -- giving a rich, order-dependent event stream.
struct CommandSpec_p17 {
    bool is_cancel = false;
    bool is_buy = true;
    std::uint64_t order_id = 0;
    std::uint64_t price_ticks = 0;
    std::uint32_t quantity = 0;  // 0 is intentionally invalid (validation reject)
};

// Generator for one command. The 1-in-4 `is_cancel` roll keeps NewOrders in the
// majority so a book actually builds up for cancels and crosses to act on.
rc::Gen<CommandSpec_p17> gen_command_p17() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange<int>(0, 4),                    // kind roll
                       rc::gen::arbitrary<bool>(),                     // side
                       rc::gen::inRange<std::uint64_t>(1, 31),         // order_id
                       rc::gen::inRange<std::uint64_t>(95, 111),       // price
                       rc::gen::inRange<std::uint32_t>(0, 41)),        // quantity
        [](const std::tuple<int, bool, std::uint64_t, std::uint64_t,
                            std::uint32_t>& t) {
            return CommandSpec_p17{/*is_cancel=*/std::get<0>(t) == 0,
                                   /*is_buy=*/std::get<1>(t),
                                   /*order_id=*/std::get<2>(t),
                                   /*price_ticks=*/std::get<3>(t),
                                   /*quantity=*/std::get<4>(t)};
        });
}

// Materialize a generated command sequence into concrete Binary_Messages,
// assigning each NewOrder a strictly increasing arrival sequence number so both
// runs see identical inputs (the seq drives time priority).
std::vector<BinaryMessage> build_messages_p17(
    const std::vector<CommandSpec_p17>& specs) {
    std::vector<BinaryMessage> msgs;
    msgs.reserve(specs.size());
    std::uint64_t seq = 1;
    for (const auto& s : specs) {
        if (s.is_cancel) {
            msgs.push_back(BinaryMessage{CancelOrder{s.order_id}});
        } else {
            NewOrder o;
            o.order_id = s.order_id;
            o.side = s.is_buy ? Side::Buy : Side::Sell;
            o.price_ticks = s.price_ticks;
            o.quantity = s.quantity;
            o.seq = seq++;
            msgs.push_back(BinaryMessage{o});
        }
    }
    return msgs;
}

// Drain every outbound event currently on the egress ring, in FIFO order.
std::vector<BinaryMessage> drain_p17(Processor_p17& proc) {
    std::vector<BinaryMessage> events;
    BinaryMessage ev;
    while (proc.next_event(ev)) {
        events.push_back(ev);
    }
    return events;
}

}  // namespace

TEST_CASE("Property 17: the engine processes ingress messages in strict FIFO order",
          "[loop][property][fifo]") {
    const bool ok = rc::check(
        "batch process_all event sequence == strict arrival-order reference",
        [] {
            const std::vector<CommandSpec_p17> specs = *rc::gen::resize(
                kMaxCommands_p17,
                rc::gen::container<std::vector<CommandSpec_p17>>(
                    gen_command_p17()));
            const std::vector<BinaryMessage> msgs = build_messages_p17(specs);

            // Reference: process strictly one-at-a-time in arrival order,
            // collecting events as they are emitted.
            Processor_p17 reference;
            std::vector<BinaryMessage> expected_events;
            for (const auto& msg : msgs) {
                RC_ASSERT(reference.submit(msg));
                RC_ASSERT(reference.process_next());
                const auto step_events = drain_p17(reference);
                expected_events.insert(expected_events.end(),
                                       step_events.begin(), step_events.end());
            }

            // Subject: enqueue the whole sequence first, then process the
            // ingress ring to exhaustion and drain the egress ring once.
            Processor_p17 subject;
            for (const auto& msg : msgs) {
                RC_ASSERT(subject.submit(msg));
            }
            const std::size_t processed = subject.process_all();
            const std::vector<BinaryMessage> actual_events = drain_p17(subject);

            // Every enqueued message was processed (none skipped) ...
            RC_ASSERT(processed == msgs.size());
            RC_ASSERT(subject.processed_count() == msgs.size());
            // ... no event was dropped to egress back-pressure in either run ...
            RC_ASSERT(subject.egress_overflow_count() == 0);
            RC_ASSERT(reference.egress_overflow_count() == 0);
            // ... and the batch event stream equals the strict arrival-order
            // reference, witnessing FIFO processing with no reordering.
            RC_ASSERT(actual_events == expected_events);
        });
    CHECK(ok);
}
