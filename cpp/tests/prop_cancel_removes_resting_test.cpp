// Cancellation removes a resting order. For any Order_Book containing a
// Resting_Order, processing a Cancel_Request for that order removes it from the
// Order_Book, emits exactly one cancellation acknowledgement containing its
// identifier, and excludes that identifier from all subsequent matching.
//
// It builds a book of resting orders (all on one side so none of them match
// each other), picks one resting order to cancel, runs
// MatchingEngine::process_cancel_order, and asserts:
//
//   * the cancel succeeds, emitting exactly one Ack carrying the cancelled
//     identifier with AckKind::Cancelled and zero Rejects, and the order is
//     gone from the book (find_order returns kNullNode);
//   * a subsequent incoming order that crosses every resting price never trades
//     against the cancelled identifier - the set of resting identifiers
//     consumed equals every still-resting identifier (all but the cancelled
//     one).
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p15` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <tuple>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/matching_engine.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::Ack;
using hme::AckKind;
using hme::CancelOrder;
using hme::NewOrder;
using hme::Reject;
using hme::Side;
using hme::Trade;
using hme::engine::kNullNode;
using hme::engine::MatchingEngine;

namespace {

// A resting-order specification produced by the generator. Identifiers are
// assigned sequentially by the test (1, 2, 3, ...) so every resting order has a
// distinct, unambiguous identifier; the generator supplies only price, arrival
// sequence, and quantity.
struct RestingSpec_p15 {
    std::uint64_t price_ticks = 0;
    std::uint64_t seq = 0;
    std::uint32_t quantity = 0;
};

// Small, overlapping domains so generated cases frequently share a price level
// (multiple orders at one price) while staying well within the book's capacity.
constexpr std::uint64_t kMinPrice_p15 = 100;
constexpr std::uint64_t kMaxPrice_p15 = 110;  // inclusive
constexpr std::uint64_t kMaxSeq_p15 = 8;      // seqs in [1, 8]
constexpr std::uint32_t kMaxQty_p15 = 50;     // qty in [1, 50]

// Generator for a single resting order's mutable fields.
rc::Gen<RestingSpec_p15> gen_resting_spec_p15() {
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::inRange<std::uint64_t>(kMinPrice_p15, kMaxPrice_p15 + 1),
            rc::gen::inRange<std::uint64_t>(1, kMaxSeq_p15 + 1),
            rc::gen::inRange<std::uint32_t>(1, kMaxQty_p15 + 1)),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t>& t) {
            return RestingSpec_p15{std::get<0>(t), std::get<1>(t),
                                   std::get<2>(t)};
        });
}

}  // namespace

TEST_CASE("Property 15: cancelling a resting order removes it, acks once, and "
          "excludes it from later matching",
          "[cancel][property][cancel-removes-resting]") {
    const bool ok = rc::check(
        "cancel removes the order, emits one Cancelled ack, and the id never "
        "matches again",
        [] {
            // All resting orders sit on the same side so none of them match
            // each other while the book is being built; the opposite side is
            // empty for each insertion.
            const bool resting_is_buy = *rc::gen::arbitrary<bool>();
            const Side resting_side =
                resting_is_buy ? Side::Buy : Side::Sell;

            // At least one resting order (so there is something to cancel), and
            // a count comfortably below the book capacity (64).
            const std::vector<RestingSpec_p15> specs = *rc::gen::resize(
                18, rc::gen::nonEmpty<std::vector<RestingSpec_p15>>(
                        rc::gen::container<std::vector<RestingSpec_p15>>(
                            gen_resting_spec_p15())));
            const std::size_t count = specs.size();

            // Assign distinct, contiguous identifiers 1..count.
            std::vector<std::uint64_t> ids(count);
            for (std::size_t i = 0; i < count; ++i) {
                ids[i] = static_cast<std::uint64_t>(i + 1);
            }

            // Choose which resting order to cancel.
            const std::size_t target_idx =
                *rc::gen::inRange<std::size_t>(0, count);
            const std::uint64_t target_id = ids[target_idx];

            // Build the book by resting every generated order.
            MatchingEngine<64, 64> engine;
            for (std::size_t i = 0; i < count; ++i) {
                NewOrder o;
                o.order_id = ids[i];
                o.side = resting_side;
                o.price_ticks = specs[i].price_ticks;
                o.quantity = specs[i].quantity;
                o.seq = specs[i].seq;
                engine.process_new_order(o, [](const Trade&) {});
            }

            // The target must be resting before the cancel.
            RC_ASSERT(engine.book().find_order(target_id) != kNullNode);

            // ---- Cancel the target order ----
            CancelOrder cancel;
            cancel.order_id = target_id;
            std::vector<Ack> acks;
            std::vector<Reject> rejects;
            const bool removed = engine.process_cancel_order(
                cancel, [&](const Ack& a) { acks.push_back(a); },
                [&](const Reject& r) { rejects.push_back(r); });

            // The cancel succeeded with exactly one Cancelled ack and no
            // rejection.
            RC_ASSERT(removed);
            RC_ASSERT(rejects.empty());
            RC_ASSERT(acks.size() == 1);
            RC_ASSERT(acks[0].order_id == target_id);
            RC_ASSERT(acks[0].kind == AckKind::Cancelled);

            // The order is removed from the book.
            RC_ASSERT(engine.book().find_order(target_id) == kNullNode);

            // ---- A subsequent crossing order excludes the cancelled id ----
            // Submit an incoming order on the opposite side, priced and sized
            // to sweep every remaining resting order. The cancelled order, if
            // it had still been resting, would also have crossed - so its
            // absence from the trades demonstrates exclusion.
            const Side incoming_side =
                resting_is_buy ? Side::Sell : Side::Buy;
            // A sell at price 1 crosses every resting bid; a buy at a very high
            // price crosses every resting ask.
            const std::uint64_t incoming_price =
                resting_is_buy ? std::uint64_t{1}
                               : std::uint64_t{1'000'000'000};
            // Total quantity is enough to fully consume all resting orders.
            std::uint64_t total_qty = 0;
            for (const auto& s : specs) {
                total_qty += s.quantity;
            }

            NewOrder incoming;
            incoming.order_id = 1'000'000;  // distinct from any resting id.
            incoming.side = incoming_side;
            incoming.price_ticks = incoming_price;
            incoming.quantity = static_cast<std::uint32_t>(total_qty);
            incoming.seq = 1'000'000;

            std::vector<Trade> trades;
            engine.process_new_order(
                incoming, [&](const Trade& t) { trades.push_back(t); });

            // The cancelled identifier never appears as a resting counterparty.
            for (const auto& t : trades) {
                RC_ASSERT(t.resting_id != target_id);
            }

            // Every other resting order was consumed exactly once: the set of
            // resting ids in the trades equals all ids except the cancelled one.
            std::set<std::uint64_t> matched_ids;
            for (const auto& t : trades) {
                matched_ids.insert(t.resting_id);
            }
            std::set<std::uint64_t> expected_ids;
            for (std::size_t i = 0; i < count; ++i) {
                if (ids[i] != target_id) {
                    expected_ids.insert(ids[i]);
                }
            }
            RC_ASSERT(matched_ids == expected_ids);
        });
    CHECK(ok);
}
