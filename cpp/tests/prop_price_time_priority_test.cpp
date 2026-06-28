// Price-time priority matching. For any Order_Book and incoming Order, the
// sequence of resting orders consumed by matching is exactly the eligible
// opposite-side orders ordered first by best price (lowest ask for a buy,
// highest bid for a sell) and, within a price level, by earliest arrival
// sequence number and then lowest order identifier; every trade price is on the
// correct side of the incoming limit price.
//
// It generates a set of resting orders all on one side plus a crossing incoming
// order on the opposite side, runs the MatchingEngine, and asserts that the
// sequence of resting order identifiers consumed by matching equals an
// independently computed price-then-(seq, id) ordering, and that every trade
// price lies on the correct side of the incoming limit price.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p8` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/matching_engine.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::NewOrder;
using hme::Side;
using hme::Trade;
using hme::engine::MatchingEngine;

namespace {

// A resting-order specification produced by the generator: the fields needed to
// place a single Limit_Order into the book and to reason about its matching
// priority.
struct RestingSpec_p8 {
    std::uint64_t order_id = 0;
    std::uint64_t price_ticks = 0;
    std::uint64_t seq = 0;
    std::uint32_t quantity = 0;
};

// Small, overlapping domains chosen so that many generated cases share a price
// level (exercising time priority) and share a (price, seq) pair (exercising
// the lower-order-id tie break), while incoming orders frequently cross.
constexpr std::uint64_t kMinPrice_p8 = 98;
constexpr std::uint64_t kMaxPrice_p8 = 108;  // inclusive
constexpr std::uint64_t kMaxSeq_p8 = 6;      // seqs in [1, 6]
constexpr std::uint32_t kMaxRestingQty_p8 = 50;

// Generator for a single resting order. order_id is drawn from a modest range:
// duplicates are harmless here because the assertion compares the *sequence of
// resting identifiers* consumed, which is derived consistently on both sides.
rc::Gen<RestingSpec_p8> gen_resting_spec_p8() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange<std::uint64_t>(1, 41),
                       rc::gen::inRange<std::uint64_t>(kMinPrice_p8,
                                                       kMaxPrice_p8 + 1),
                       rc::gen::inRange<std::uint64_t>(1, kMaxSeq_p8 + 1),
                       rc::gen::inRange<std::uint32_t>(1, kMaxRestingQty_p8 + 1)),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                            std::uint32_t>& t) {
            return RestingSpec_p8{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                                  std::get<3>(t)};
        });
}

// True when an incoming order on `incoming_side` priced at `incoming_price` is
// eligible to trade against a resting order priced at `resting_price`:
// a buy crosses asks at or below it; a sell crosses bids at or above it.
bool crosses_p8(Side incoming_side, std::uint64_t incoming_price,
                std::uint64_t resting_price) {
    return (incoming_side == Side::Buy) ? (resting_price <= incoming_price)
                                        : (resting_price >= incoming_price);
}

// The price-time priority order in which the engine must consume eligible
// resting orders: best price first (lowest ask for a buy, highest bid for a
// sell), then earliest seq, then lowest order id. A stable sort preserves
// arrival (insertion) order for fully-tied keys, matching the book's FIFO
// behaviour for identical (seq, id).
bool priority_less_p8(Side incoming_side, const RestingSpec_p8& a,
                      const RestingSpec_p8& b) {
    if (a.price_ticks != b.price_ticks) {
        return (incoming_side == Side::Buy) ? (a.price_ticks < b.price_ticks)
                                            : (a.price_ticks > b.price_ticks);
    }
    if (a.seq != b.seq) {
        return a.seq < b.seq;
    }
    return a.order_id < b.order_id;
}

}  // namespace

TEST_CASE("Property 8: matching consumes resting orders in price-time priority",
          "[match][property][price-time-priority]") {
    const bool ok = rc::check(
        "consumed resting-id sequence == price-then-(seq,id) ordering",
        [] {
            // Sample the inputs from the local generators. The resting-order
            // count is capped well below the book's capacity (64) so every
            // generated order rests successfully.
            const bool incoming_is_buy = *rc::gen::arbitrary<bool>();
            const std::vector<RestingSpec_p8> resting =
                *rc::gen::resize(30, rc::gen::container<std::vector<RestingSpec_p8>>(
                                         gen_resting_spec_p8()));
            const std::uint64_t incoming_price =
                *rc::gen::inRange<std::uint64_t>(kMinPrice_p8 - 2,
                                                 kMaxPrice_p8 + 3);
            const std::uint32_t in_qty =
                *rc::gen::inRange<std::uint32_t>(1, 401);  // [1, 400]

            const Side incoming_side =
                incoming_is_buy ? Side::Buy : Side::Sell;
            // Resting orders all sit on the side opposite the incoming order so
            // they rest without matching each other and are eligible
            // counterparties for the incoming order.
            const Side resting_side =
                incoming_is_buy ? Side::Sell : Side::Buy;

            // Build the book by resting every generated order. The opposite
            // side is empty for each insertion, so none of them match.
            MatchingEngine<64, 64> engine;
            for (const auto& s : resting) {
                NewOrder o;
                o.order_id = s.order_id;
                o.side = resting_side;
                o.price_ticks = s.price_ticks;
                o.quantity = s.quantity;
                o.seq = s.seq;
                engine.process_new_order(o, [](const Trade&) {});
            }

            // Run the incoming crossing order, recording each trade in order.
            NewOrder incoming;
            incoming.order_id = 100000;  // distinct from any resting id.
            incoming.side = incoming_side;
            incoming.price_ticks = incoming_price;
            incoming.quantity = in_qty;
            incoming.seq = 100000;

            std::vector<Trade> trades;
            engine.process_new_order(
                incoming, [&](const Trade& t) { trades.push_back(t); });

            // Independently compute the expected consumed resting-id sequence:
            // sort the eligible resting orders by price-time priority and walk
            // them, consuming the incoming quantity.
            std::vector<RestingSpec_p8> eligible;
            for (const auto& s : resting) {
                if (crosses_p8(incoming_side, incoming_price, s.price_ticks)) {
                    eligible.push_back(s);
                }
            }
            std::stable_sort(eligible.begin(), eligible.end(),
                             [&](const RestingSpec_p8& a,
                                 const RestingSpec_p8& b) {
                                 return priority_less_p8(incoming_side, a, b);
                             });

            std::vector<std::uint64_t> expected_ids;
            std::uint32_t remaining = in_qty;
            for (const auto& s : eligible) {
                if (remaining == 0) {
                    break;
                }
                expected_ids.push_back(s.order_id);
                const std::uint32_t traded = std::min(remaining, s.quantity);
                remaining -= traded;
            }

            // Actual consumed resting-id sequence from the engine's trades.
            std::vector<std::uint64_t> actual_ids;
            actual_ids.reserve(trades.size());
            for (const auto& t : trades) {
                actual_ids.push_back(t.resting_id);
            }

            // Core ordering guarantee.
            RC_ASSERT(actual_ids == expected_ids);

            // Every trade price is on the correct side of the incoming limit
            // and is the resting order's price (best-price eligibility).
            for (const auto& t : trades) {
                if (incoming_side == Side::Buy) {
                    RC_ASSERT(t.price_ticks <= incoming_price);
                } else {
                    RC_ASSERT(t.price_ticks >= incoming_price);
                }
            }
        });
    CHECK(ok);
}
