// Incoming order terminal condition. For any incoming Order, after the
// Matching_Engine finishes processing it, either the incoming Order's remaining
// quantity is zero or no eligible opposite-side Resting_Order remains in the
// Order_Book.
//
// It builds an arbitrary (internally consistent) Order_Book by processing a
// generated sequence of NewOrders, then processes one final incoming crossing
// order and asserts the terminal condition: matching stopped ONLY because the
// incoming order was fully filled (no remainder rested) OR because no
// opposite-side resting order still crosses the incoming limit price.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p10` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

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
using hme::engine::kNullNode;
using hme::engine::MatchingEngine;
using hme::engine::PriceLevel;

namespace {

// A NewOrder specification produced by the generator: the fields needed to
// submit a single order to the engine while building up the book.
struct OrderSpec_p10 {
    std::uint64_t order_id = 0;
    bool is_buy = true;
    std::uint64_t price_ticks = 0;
    std::uint32_t quantity = 0;
};

// Small, overlapping domains chosen so that book-building orders frequently
// rest on both sides and the final incoming order frequently crosses them.
constexpr std::uint64_t kMinPrice_p10 = 98;
constexpr std::uint64_t kMaxPrice_p10 = 108;  // inclusive
constexpr std::uint32_t kMaxQty_p10 = 60;

// Generator for a single book-building order. order_ids are drawn from a modest
// range; duplicates among resting orders are harmless because the terminal-
// condition assertion only queries the distinct incoming identifier.
rc::Gen<OrderSpec_p10> gen_order_spec_p10() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange<std::uint64_t>(1, 81),
                       rc::gen::arbitrary<bool>(),
                       rc::gen::inRange<std::uint64_t>(kMinPrice_p10,
                                                       kMaxPrice_p10 + 1),
                       rc::gen::inRange<std::uint32_t>(1, kMaxQty_p10 + 1)),
        [](const std::tuple<std::uint64_t, bool, std::uint64_t, std::uint32_t>&
               t) {
            return OrderSpec_p10{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                                 std::get<3>(t)};
        });
}

// True when an incoming order on `incoming_side` priced at `incoming_price` is
// eligible to trade against a resting order priced at `resting_price`: a buy
// crosses asks at or below it; a sell crosses bids at or above it. Mirrors the
// engine's own eligibility rule.
bool crosses_p10(Side incoming_side, std::uint64_t incoming_price,
                 std::uint64_t resting_price) {
    return (incoming_side == Side::Buy) ? (resting_price <= incoming_price)
                                        : (resting_price >= incoming_price);
}

}  // namespace

TEST_CASE(
    "Property 10: after processing, incoming is filled or no crossing "
    "counterpart remains",
    "[match][property][terminal-condition]") {
    const bool ok = rc::check(
        "incoming fully filled OR no eligible opposite-side order remains",
        [] {
            // Build an arbitrary, internally consistent book by processing a
            // generated sequence of NewOrders. Counts stay well below the
            // book's capacity so every remainder rests successfully (no pool
            // exhaustion that could masquerade as a full fill).
            const std::vector<OrderSpec_p10> book_orders =
                *rc::gen::resize(20, rc::gen::container<std::vector<OrderSpec_p10>>(
                                         gen_order_spec_p10()));

            const bool incoming_is_buy = *rc::gen::arbitrary<bool>();
            const std::uint64_t incoming_price =
                *rc::gen::inRange<std::uint64_t>(kMinPrice_p10 - 2,
                                                 kMaxPrice_p10 + 3);
            const std::uint32_t incoming_qty =
                *rc::gen::inRange<std::uint32_t>(1, 401);  // [1, 400]

            MatchingEngine<256, 256> engine;

            // Process the book-building orders. They match against one another
            // as they arrive, leaving the book in a consistent (non-crossing)
            // state; the resulting trades are irrelevant to this property.
            std::uint64_t seq = 1;
            for (const auto& s : book_orders) {
                NewOrder o;
                o.order_id = s.order_id;
                o.side = s.is_buy ? Side::Buy : Side::Sell;
                o.price_ticks = s.price_ticks;
                o.quantity = s.quantity;
                o.seq = seq++;
                engine.process_new_order(o, [](const Trade&) {});
            }

            // Process one final incoming order. Its identifier is distinct from
            // every book-building identifier so we can detect whether any
            // remainder rested.
            constexpr std::uint64_t kIncomingId = 1'000'000;
            NewOrder incoming;
            incoming.order_id = kIncomingId;
            incoming.side = incoming_is_buy ? Side::Buy : Side::Sell;
            incoming.price_ticks = incoming_price;
            incoming.quantity = incoming_qty;
            incoming.seq = seq;
            engine.process_new_order(incoming, [](const Trade&) {});

            const auto& book = engine.book();

            // Determine which terminal branch the engine landed in: the incoming
            // order is fully filled iff none of its remainder rested in the
            // book (its identifier is absent).
            const bool incoming_fully_filled =
                book.find_order(kIncomingId) == kNullNode;

            if (incoming_fully_filled) {
                // First disjunct holds: remaining quantity is zero. Nothing
                // further to require about the opposite side.
                return;
            }

            // The incoming order rested a positive remainder, so matching must
            // have stopped because no eligible opposite-side counterpart
            // remained. The best opposite-side level is the most aggressive
            // price on that side; if it does not cross, no level on that side
            // crosses.
            const PriceLevel* best_opposite =
                incoming_is_buy ? book.best_ask() : book.best_bid();
            if (best_opposite != nullptr) {
                RC_ASSERT(!crosses_p10(incoming.side, incoming_price,
                                       best_opposite->price_ticks));
            }
        });
    CHECK(ok);
}
