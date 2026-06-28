// Trade pricing, sizing, and event completeness. For any match between an
// incoming Order and a Resting_Order, the resulting Trade executes at the
// Resting_Order's limit price, has a quantity equal to the smaller of the two
// orders' remaining quantities, has a quantity strictly greater than zero, and
// is emitted as a Trade event populated with the execution sequence number,
// price, quantity, incoming order identifier, and resting order identifier.
//
// It rests a set of orders on one side of the book, runs a crossing incoming
// order on the opposite side, and checks each emitted Trade against an
// independently computed step-by-step expectation:
//
//   * the Trade price equals the consumed Resting_Order's limit price;
//   * the Trade quantity equals min(incoming remaining, resting remaining) at
//     that matching step and is strictly positive;
//   * every Trade carries the incoming order id and the resting order id, and
//     the exec_seq field is complete and strictly increasing across the run.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p9` so they do not clash (ODR) with generators in other property-test TUs.

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

// A resting-order specification produced by the generator. order_id is assigned
// separately (unique, increasing with insertion) so the matching priority is
// unambiguous and the expected resting-id sequence is well defined.
struct RestingSpec_p9 {
    std::uint64_t price_ticks = 0;
    std::uint64_t seq = 0;
    std::uint32_t quantity = 0;
};

// Small, overlapping domains so generated cases frequently share a price level
// (exercising time priority) and a (price, seq) pair (exercising the
// lower-order-id tie break), while incoming orders frequently cross and partial
// fills occur.
constexpr std::uint64_t kMinPrice_p9 = 98;
constexpr std::uint64_t kMaxPrice_p9 = 108;  // inclusive
constexpr std::uint64_t kMaxSeq_p9 = 6;      // seqs in [1, 6]
constexpr std::uint32_t kMaxRestingQty_p9 = 50;

// Generator for a single resting order's price/seq/quantity.
rc::Gen<RestingSpec_p9> gen_resting_spec_p9() {
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::inRange<std::uint64_t>(kMinPrice_p9, kMaxPrice_p9 + 1),
            rc::gen::inRange<std::uint64_t>(1, kMaxSeq_p9 + 1),
            rc::gen::inRange<std::uint32_t>(1, kMaxRestingQty_p9 + 1)),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t>& t) {
            return RestingSpec_p9{std::get<0>(t), std::get<1>(t),
                                  std::get<2>(t)};
        });
}

// True when an incoming order on `incoming_side` priced at `incoming_price` is
// eligible to trade against a resting order priced at `resting_price`: a buy
// crosses asks at or below it; a sell crosses bids at or above it.
bool crosses_p9(Side incoming_side, std::uint64_t incoming_price,
                std::uint64_t resting_price) {
    return (incoming_side == Side::Buy) ? (resting_price <= incoming_price)
                                        : (resting_price >= incoming_price);
}

// A resting order with its assigned identifier, used to compute the expected
// matching order independently of the engine.
struct PricedOrder_p9 {
    std::uint64_t order_id = 0;
    std::uint64_t price_ticks = 0;
    std::uint64_t seq = 0;
    std::uint32_t quantity = 0;
};

// Price-time priority: best price first (lowest ask for a buy, highest bid for
// a sell), then earliest seq, then lowest order id. A stable sort preserves
// insertion order for fully-tied keys, mirroring the book's FIFO behaviour.
bool priority_less_p9(Side incoming_side, const PricedOrder_p9& a,
                      const PricedOrder_p9& b) {
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

TEST_CASE(
    "Property 9: trades price at resting limit, size as min remaining, and "
    "carry complete fields",
    "[match][property][trade-pricing]") {
    const bool ok = rc::check(
        "each Trade: price==resting, qty==min remaining>0, fields complete, "
        "exec_seq strictly increasing",
        [] {
            const bool incoming_is_buy = *rc::gen::arbitrary<bool>();
            const std::vector<RestingSpec_p9> resting =
                *rc::gen::resize(
                    30, rc::gen::container<std::vector<RestingSpec_p9>>(
                            gen_resting_spec_p9()));
            const std::uint64_t incoming_price =
                *rc::gen::inRange<std::uint64_t>(kMinPrice_p9 - 2,
                                                 kMaxPrice_p9 + 3);
            const std::uint32_t in_qty =
                *rc::gen::inRange<std::uint32_t>(1, 401);  // [1, 400]

            const Side incoming_side = incoming_is_buy ? Side::Buy : Side::Sell;
            // Resting orders all sit on the side opposite the incoming order so
            // they rest without matching each other and are eligible
            // counterparties for the incoming order.
            const Side resting_side = incoming_is_buy ? Side::Sell : Side::Buy;

            // Build the book by resting every generated order, assigning a
            // unique, increasing identifier per insertion so the expected
            // matching order (and resting-id sequence) is unambiguous.
            MatchingEngine<64, 64> engine;
            std::vector<PricedOrder_p9> placed;
            placed.reserve(resting.size());
            std::uint64_t next_id = 1;
            for (const auto& s : resting) {
                const std::uint64_t id = next_id++;
                NewOrder o;
                o.order_id = id;
                o.side = resting_side;
                o.price_ticks = s.price_ticks;
                o.quantity = s.quantity;
                o.seq = s.seq;
                engine.process_new_order(o, [](const Trade&) {});
                placed.push_back(
                    PricedOrder_p9{id, s.price_ticks, s.seq, s.quantity});
            }

            // Run the incoming crossing order, recording each emitted trade.
            const std::uint64_t incoming_id = 100000;  // distinct from resting.
            NewOrder incoming;
            incoming.order_id = incoming_id;
            incoming.side = incoming_side;
            incoming.price_ticks = incoming_price;
            incoming.quantity = in_qty;
            incoming.seq = 100000;

            const std::uint64_t exec_seq_before = engine.next_exec_seq();

            std::vector<Trade> trades;
            engine.process_new_order(
                incoming, [&](const Trade& t) { trades.push_back(t); });

            // Independently compute the expected sequence of trades: the
            // eligible resting orders in price-time priority, each consumed by
            // min(incoming remaining, resting remaining) until the incoming
            // order is exhausted.
            std::vector<PricedOrder_p9> eligible;
            for (const auto& p : placed) {
                if (crosses_p9(incoming_side, incoming_price, p.price_ticks)) {
                    eligible.push_back(p);
                }
            }
            std::stable_sort(
                eligible.begin(), eligible.end(),
                [&](const PricedOrder_p9& a, const PricedOrder_p9& b) {
                    return priority_less_p9(incoming_side, a, b);
                });

            struct ExpectedTrade_p9 {
                std::uint64_t price_ticks;
                std::uint32_t quantity;
                std::uint64_t resting_id;
            };
            std::vector<ExpectedTrade_p9> expected;
            std::uint32_t remaining = in_qty;
            for (const auto& p : eligible) {
                if (remaining == 0) {
                    break;
                }
                const std::uint32_t traded = std::min(remaining, p.quantity);
                expected.push_back(
                    ExpectedTrade_p9{p.price_ticks, traded, p.order_id});
                remaining -= traded;
            }

            // The number of emitted trades matches the independent expectation.
            RC_ASSERT(trades.size() == expected.size());

            std::uint64_t prev_exec_seq = exec_seq_before;
            for (std::size_t i = 0; i < trades.size(); ++i) {
                const Trade& t = trades[i];
                const ExpectedTrade_p9& e = expected[i];

                // The Trade executes at the resting order's limit price.
                RC_ASSERT(t.price_ticks == e.price_ticks);

                // Quantity is the smaller of the two remaining quantities at
                // this matching step.
                RC_ASSERT(t.quantity == e.quantity);

                // Quantity is strictly positive.
                RC_ASSERT(t.quantity > 0);

                // The Trade carries the incoming and resting order identifiers.
                RC_ASSERT(t.incoming_id == incoming_id);
                RC_ASSERT(t.resting_id == e.resting_id);

                // The execution sequence number is complete and strictly
                // increasing across the emitted trades. The first emitted Trade
                // takes the engine's next execution sequence number (captured
                // before processing); each subsequent Trade strictly increases
                // it.
                if (i == 0) {
                    RC_ASSERT(t.exec_seq == exec_seq_before);
                } else {
                    RC_ASSERT(t.exec_seq > prev_exec_seq);
                }
                prev_exec_seq = t.exec_seq;
            }
        });
    CHECK(ok);
}
