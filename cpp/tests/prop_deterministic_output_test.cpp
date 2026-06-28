// Deterministic output sequence. For any initial Order_Book state and any
// sequence of input messages, two independent runs from that initial state
// produce output event sequences that are identical in both content and
// ordering, independent of wall-clock time, inter-arrival timing, and host
// load.
//
// It generates one sequence of input messages (a mix of NewOrder and
// CancelOrder commands, spanning valid and invalid orders so the
// rejection/continue-on-error path is exercised too), feeds that SAME sequence
// into two independently constructed, freshly initialized EngineProcessor
// instances, drains each processor's egress event ring in FIFO order, and
// asserts the two drained event sequences are identical in length, ordering,
// and every field (including exec/seq numbers). Equality relies on
// BinaryMessage's defaulted operator== across all variants, so
// std::vector<BinaryMessage> comparison is exact.
//
// Both processors start from the same empty initial book and consume the same
// command sequence, with no clock reads or timing dependence anywhere in the
// engine; determinism therefore means the two egress sequences must be equal.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p18` so they do not clash (ODR) with generators in other property-test TUs.

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

// A single generated input command: either a NewOrder (is_cancel == false) or a
// CancelOrder (is_cancel == true). The NewOrder fields are only meaningful for
// a NewOrder; a CancelOrder uses only order_id.
struct CommandSpec_p18 {
    bool is_cancel = false;
    std::uint64_t order_id = 0;
    Side side = Side::Buy;
    std::uint64_t price_ticks = 0;
    std::uint32_t quantity = 0;
    std::uint64_t seq = 0;
};

// Small, overlapping domains so generated sequences interact: order ids repeat
// (cancels frequently reference resting orders), prices cluster on one band so
// crossing/matching is common, and quantities include 0 to drive the invalid-
// order rejection / continue-on-error path within the deterministic stream.
constexpr std::uint64_t kMinOrderId_p18 = 1;
constexpr std::uint64_t kMaxOrderId_p18 = 16;  // inclusive
constexpr std::uint64_t kMinPrice_p18 = 98;
constexpr std::uint64_t kMaxPrice_p18 = 106;  // inclusive
constexpr std::uint32_t kMaxQty_p18 = 40;     // qty in [0, 40]; 0 is invalid

// Generator for a single command. The monotonic arrival sequence number is
// assigned afterwards (see gen_command_sequence_p18) so it strictly increases
// across the whole stream regardless of command type.
rc::Gen<CommandSpec_p18> gen_command_p18() {
    return rc::gen::map(
        rc::gen::tuple(
            // ~30% cancels, ~70% new orders.
            rc::gen::weightedOneOf<bool>(
                {{7, rc::gen::just(false)}, {3, rc::gen::just(true)}}),
            rc::gen::inRange<std::uint64_t>(kMinOrderId_p18, kMaxOrderId_p18 + 1),
            rc::gen::arbitrary<bool>(),
            rc::gen::inRange<std::uint64_t>(kMinPrice_p18, kMaxPrice_p18 + 1),
            rc::gen::inRange<std::uint32_t>(0, kMaxQty_p18 + 1)),
        [](const std::tuple<bool, std::uint64_t, bool, std::uint64_t,
                            std::uint32_t>& t) {
            CommandSpec_p18 c;
            c.is_cancel = std::get<0>(t);
            c.order_id = std::get<1>(t);
            c.side = std::get<2>(t) ? Side::Buy : Side::Sell;
            c.price_ticks = std::get<3>(t);
            c.quantity = std::get<4>(t);
            c.seq = 0;  // filled in by the sequence generator.
            return c;
        });
}

// Generator for a whole command sequence with monotonically increasing arrival
// sequence numbers (1, 2, 3, ...). The sequence length is capped well below the
// processor's ingress capacity so every command enqueues successfully.
rc::Gen<std::vector<CommandSpec_p18>> gen_command_sequence_p18() {
    return rc::gen::map(
        rc::gen::container<std::vector<CommandSpec_p18>>(gen_command_p18()),
        [](std::vector<CommandSpec_p18> cmds) {
            std::uint64_t next_seq = 1;
            for (auto& c : cmds) {
                c.seq = next_seq++;
            }
            return cmds;
        });
}

// Capacities sized generously relative to the capped command-sequence length so
// neither the ingress ring nor the egress ring overflows: a NewOrder can emit
// several Trade events (one per resting order it consumes), so the egress ring
// is the largest.
using Processor_p18 = EngineProcessor<256, 256, 256, 1024>;

// Run one command sequence through a fresh, freshly-initialized processor and
// return the drained egress event sequence in FIFO order.
std::vector<BinaryMessage> run_sequence_p18(
    const std::vector<CommandSpec_p18>& cmds) {
    Processor_p18 proc;
    proc.initialize();  // reserve memory, start from a clean empty book.

    for (const auto& c : cmds) {
        if (c.is_cancel) {
            proc.submit(CancelOrder{c.order_id});
        } else {
            NewOrder o;
            o.order_id = c.order_id;
            o.side = c.side;
            o.price_ticks = c.price_ticks;
            o.quantity = c.quantity;
            o.seq = c.seq;
            proc.submit(o);
        }
    }

    proc.process_all();

    std::vector<BinaryMessage> events;
    BinaryMessage ev;
    while (proc.next_event(ev)) {
        events.push_back(ev);
    }
    return events;
}

}  // namespace

TEST_CASE("Property 18: two independent runs produce identical output sequences",
          "[loop][property][determinism]") {
    const bool ok = rc::check(
        "identical input sequence -> identical egress event sequence",
        [] {
            const std::vector<CommandSpec_p18> cmds =
                *rc::gen::resize(40, gen_command_sequence_p18());

            // Two independent fresh processors consume the SAME input sequence.
            const std::vector<BinaryMessage> first = run_sequence_p18(cmds);
            const std::vector<BinaryMessage> second = run_sequence_p18(cmds);

            // Determinism: the egress event sequences must be identical in
            // length, ordering, and every field (including exec_seq),
            // independent of wall-clock time and host load. BinaryMessage's
            // defaulted operator== compares all variants.
            RC_ASSERT(first == second);
        });
    CHECK(ok);
}
