// Continue deterministically after an invalid message. For any input sequence
// that interleaves valid and invalid messages, the Matching_Engine emits
// exactly one error event per invalid message and processes the remaining valid
// messages in their original order, producing the same result as if the invalid
// messages were simply skipped.
//
// It generates a mixed stream of valid commands (in-range NewOrders and
// CancelOrders) and invalid messages (out-of-range NewOrders and out-of-place
// outbound variants placed on the ingress ring), drives them through an
// EngineProcessor one at a time, and asserts that:
//
//   * each invalid message emits exactly ONE error event (a Reject) and
//     increments error_event_count() by exactly one, so the total error count
//     equals the number of invalid messages; and
//
//   * the events produced by the valid messages in the mixed run are identical,
//     in content and order, to the events produced by running ONLY the valid
//     subsequence on a fresh processor.
//
// Together these show an invalid message never disturbs the deterministic
// processing of the messages around it.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p19` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstdint>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/engine_processor.hpp"
#include "hme/wire_protocol.hpp"

using hme::Ack;
using hme::AckKind;
using hme::BinaryMessage;
using hme::CancelOrder;
using hme::NewOrder;
using hme::Reject;
using hme::RejectReason;
using hme::Side;
using hme::Trade;
using hme::engine::EngineProcessor;

namespace {

// Storage is sized comfortably above the capped stream length so every valid
// resting order finds room. Only one message is in flight at a time (we submit,
// process, and drain per message), so the ingress capacity can stay small while
// the egress holds the events a single sweeping order can produce.
using Processor_p19 = EngineProcessor<128, 128, 16, 128>;

// The kind of message a generated item expands into. The first two are valid
// commands; the last three are invalid messages that must each yield exactly
// one error event without touching the book.
enum class Kind_p19 : std::uint8_t {
    ValidNew = 0,     // an in-range NewOrder (rests or crosses normally).
    ValidCancel = 1,  // a CancelOrder (Ack if resting, plain Reject if not).
    InvalidQty = 2,   // a NewOrder whose quantity is outside [1, 1,000,000].
    InvalidPrice = 3,  // a NewOrder whose price is outside the tick range.
    InvalidStray = 4,  // an outbound variant (Ack/Reject/Trade) on ingress.
};

// A generated, abstract item. Concrete order ids and sequence numbers are
// assigned deterministically when the stream is materialized, keeping resting
// order ids unique while still letting the structure vary.
struct ItemSpec_p19 {
    Kind_p19 kind = Kind_p19::ValidNew;
    bool buy = true;
    std::uint64_t price = 0;     // valid-new price (small crossing band).
    std::uint32_t qty = 1;       // valid-new quantity.
    std::uint32_t selector = 0;  // cancel-target / variant / out-of-range pick.
};

// Small overlapping price band so generated orders frequently cross and rest at
// shared levels, exercising matching around the invalid messages.
constexpr std::uint64_t kMinPrice_p19 = 98;
constexpr std::uint64_t kMaxPrice_p19 = 108;  // inclusive
constexpr std::uint32_t kMaxQty_p19 = 50;

// Generator for one abstract item. The kind is drawn from [0,10) and mapped so
// that roughly 60% of items are valid commands and 40% are invalid messages,
// giving every run a healthy interleaving of both.
rc::Gen<ItemSpec_p19> gen_item_p19() {
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::inRange<int>(0, 10),
            rc::gen::arbitrary<bool>(),
            rc::gen::inRange<std::uint64_t>(kMinPrice_p19, kMaxPrice_p19 + 1),
            rc::gen::inRange<std::uint32_t>(1, kMaxQty_p19 + 1),
            rc::gen::inRange<std::uint32_t>(0, 100)),
        [](const std::tuple<int, bool, std::uint64_t, std::uint32_t,
                            std::uint32_t>& t) {
            const int k = std::get<0>(t);
            Kind_p19 kind;
            if (k < 4) {
                kind = Kind_p19::ValidNew;      // 0-3
            } else if (k < 6) {
                kind = Kind_p19::ValidCancel;   // 4-5
            } else if (k < 8) {
                kind = Kind_p19::InvalidQty;    // 6-7
            } else if (k < 9) {
                kind = Kind_p19::InvalidPrice;  // 8
            } else {
                kind = Kind_p19::InvalidStray;  // 9
            }
            return ItemSpec_p19{kind, std::get<1>(t), std::get<2>(t),
                                std::get<3>(t), std::get<4>(t)};
        });
}

// A materialized stream item: whether it is an invalid message (one that must
// produce exactly one error event) and the concrete Binary_Message to submit.
struct StreamItem_p19 {
    bool invalid = false;
    BinaryMessage msg;
};

// Materialize the abstract specs into concrete messages, assigning unique,
// monotonically increasing order ids and arrival sequence numbers. Both the
// mixed run and the valid-only run share this single list, so valid items are
// byte-identical across the two runs.
std::vector<StreamItem_p19> build_stream_p19(
    const std::vector<ItemSpec_p19>& specs) {
    std::vector<StreamItem_p19> items;
    items.reserve(specs.size());

    std::uint64_t next_id = 1;
    std::uint64_t next_seq = 1;
    std::vector<std::uint64_t> valid_new_ids;  // ids that were rested as valid.

    for (const auto& s : specs) {
        const Side side = s.buy ? Side::Buy : Side::Sell;
        switch (s.kind) {
            case Kind_p19::ValidNew: {
                NewOrder o;
                o.order_id = next_id++;
                o.side = side;
                o.price_ticks = s.price;
                o.quantity = s.qty;
                o.seq = next_seq++;
                valid_new_ids.push_back(o.order_id);
                items.push_back({false, BinaryMessage{o}});
                break;
            }
            case Kind_p19::ValidCancel: {
                // Target one of the previously created order ids (so the cancel
                // often hits a resting order), or, when the selector lands past
                // the end, an id that was never used (a plain not-found cancel).
                // Both outcomes are *valid* commands: a not-found cancellation
                // is a normal Reject, never an invalid-message error event.
                std::uint64_t target = 9'000'000ULL + s.selector;
                if (!valid_new_ids.empty()) {
                    const std::size_t idx =
                        s.selector % (valid_new_ids.size() + 1);
                    if (idx < valid_new_ids.size()) {
                        target = valid_new_ids[idx];
                    }
                }
                items.push_back({false, BinaryMessage{CancelOrder{target}}});
                break;
            }
            case Kind_p19::InvalidQty: {
                NewOrder o;
                o.order_id = next_id++;
                o.side = side;
                o.price_ticks = s.price;  // in range; quantity is the problem.
                // Either below the minimum (0) or above the maximum; both fail
                // engine validation.
                o.quantity = (s.selector % 2 == 0)
                                 ? 0U
                                 : (hme::limits::kMaxEngineQuantity + 1U);
                o.seq = next_seq++;
                items.push_back({true, BinaryMessage{o}});
                break;
            }
            case Kind_p19::InvalidPrice: {
                NewOrder o;
                o.order_id = next_id++;
                o.side = side;
                // Either below the minimum tick (0) or above the maximum; both
                // fail engine validation.
                o.price_ticks = (s.selector % 2 == 0)
                                    ? 0ULL
                                    : (hme::limits::kMaxPriceTicks + 1ULL);
                o.quantity = s.qty;  // in range; price is the problem.
                o.seq = next_seq++;
                items.push_back({true, BinaryMessage{o}});
                break;
            }
            case Kind_p19::InvalidStray: {
                // An outbound event variant has no place on the ingress ring;
                // the processor must treat it as one invalid message.
                BinaryMessage stray;
                switch (s.selector % 3) {
                    case 0: {
                        Ack a;
                        a.order_id = 5'000'000ULL + s.selector;
                        a.kind = AckKind::Accepted;
                        stray = BinaryMessage{a};
                        break;
                    }
                    case 1: {
                        Reject r;
                        r.order_id = 5'000'000ULL + s.selector;
                        r.reason = RejectReason::InvalidPrice;
                        stray = BinaryMessage{r};
                        break;
                    }
                    default: {
                        Trade tr;
                        tr.incoming_id = 5'000'000ULL + s.selector;
                        stray = BinaryMessage{tr};
                        break;
                    }
                }
                items.push_back({true, std::move(stray)});
                break;
            }
        }
    }
    return items;
}

// Drain every outbound event currently on the egress ring, in FIFO order.
std::vector<BinaryMessage> drain_p19(Processor_p19& proc) {
    std::vector<BinaryMessage> events;
    BinaryMessage ev;
    while (proc.next_event(ev)) {
        events.push_back(ev);
    }
    return events;
}

}  // namespace

TEST_CASE(
    "Property 19: invalid messages never disturb processing of the valid ones",
    "[loop][property][continue-on-error]") {
    const bool ok = rc::check(
        "one error event per invalid message; valid events match the skipped "
        "run",
        [] {
            const auto specs = *rc::gen::resize(
                30, rc::gen::container<std::vector<ItemSpec_p19>>(
                        gen_item_p19()));
            const std::vector<StreamItem_p19> items = build_stream_p19(specs);

            std::uint64_t invalid_count = 0;
            for (const auto& it : items) {
                if (it.invalid) {
                    ++invalid_count;
                }
            }

            // ---- Mixed run: submit, process, and inspect one message at a
            // time so each invalid message's single error event is verified in
            // place, and the valid messages' events are collected in order.
            Processor_p19 mixed;
            std::vector<BinaryMessage> mixed_valid_events;
            for (const auto& it : items) {
                const std::uint64_t errs_before = mixed.error_event_count();
                RC_ASSERT(mixed.submit(it.msg));
                RC_ASSERT(mixed.process_next());
                std::vector<BinaryMessage> evs = drain_p19(mixed);

                if (it.invalid) {
                    // Exactly one error event per invalid message, and that
                    // single emitted event is a Reject.
                    RC_ASSERT(mixed.error_event_count() == errs_before + 1);
                    RC_ASSERT(evs.size() == 1);
                    RC_ASSERT(std::holds_alternative<Reject>(evs[0]));
                } else {
                    // A valid command never counts as an invalid-message error
                    // (a not-found cancel is a normal Reject, not an error).
                    RC_ASSERT(mixed.error_event_count() == errs_before);
                    for (auto& ev : evs) {
                        mixed_valid_events.push_back(std::move(ev));
                    }
                }
            }

            // Total error events == number of invalid messages.
            RC_ASSERT(mixed.error_event_count() == invalid_count);

            // ---- Skipped run: process ONLY the valid subsequence on a fresh
            // processor, collecting its events in order.
            Processor_p19 valid_only;
            std::vector<BinaryMessage> valid_only_events;
            for (const auto& it : items) {
                if (it.invalid) {
                    continue;
                }
                RC_ASSERT(valid_only.submit(it.msg));
                RC_ASSERT(valid_only.process_next());
                std::vector<BinaryMessage> evs = drain_p19(valid_only);
                for (auto& ev : evs) {
                    valid_only_events.push_back(std::move(ev));
                }
            }

            // The valid messages produce the same events, in the same order,
            // whether or not the invalid messages were interleaved: an invalid
            // message never disturbs the deterministic processing around it.
            RC_ASSERT(mixed_valid_events == valid_only_events);
            RC_ASSERT(valid_only.error_event_count() == 0);
        });
    CHECK(ok);
}
