// Quantity conservation. For any incoming Order accepted for matching, the
// incoming quantity equals the sum of all Trade quantities generated for that
// Order plus the quantity inserted into the Order_Book as a Resting_Order.
//
// It builds an arbitrary (internally consistent) Order_Book by processing a
// generated sequence of NewOrders, snapshots the total resting quantity, then
// processes one final incoming order while summing the quantities of every
// emitted Trade. It then asserts two complementary conservation facts:
//
//   1. Per-order conservation: the incoming order's quantity equals the total
//      traded quantity plus the quantity that rested in the book as the
//      incoming order's remainder.
//
//   2. System conservation: the reduction in total resting quantity on the
//      counterparty side equals the total traded quantity. Equivalently, the
//      total quantity in the book after the operation equals the total before,
//      plus the incoming quantity, minus twice the traded quantity (each Trade
//      removes the traded amount from a resting order AND from the incoming
//      order). No quantity is created or destroyed by matching.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p11` so they do not clash (ODR) with generators in other property-test TUs.

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
using hme::engine::NodeIndex;
using hme::engine::PriceLevel;

namespace {

// A NewOrder specification produced by the generator: the fields needed to
// submit a single book-building order to the engine.
struct OrderSpec_p11 {
    std::uint64_t order_id = 0;
    bool is_buy = true;
    std::uint64_t price_ticks = 0;
    std::uint32_t quantity = 0;
};

// Small, overlapping domains chosen so book-building orders frequently rest on
// both sides and the final incoming order frequently crosses them, exercising
// full fills, partial fills, and no-match resting. order_ids stay in [1, 81],
// disjoint from the distinct incoming identifier used below.
constexpr std::uint64_t kMinPrice_p11 = 98;
constexpr std::uint64_t kMaxPrice_p11 = 108;  // inclusive
constexpr std::uint32_t kMaxQty_p11 = 60;

// Generator for a single book-building order. Duplicate order_ids among resting
// orders are harmless here: the conservation arithmetic sums quantities over
// the whole book and isolates the incoming remainder by its distinct id.
rc::Gen<OrderSpec_p11> gen_order_spec_p11() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange<std::uint64_t>(1, 81),
                       rc::gen::arbitrary<bool>(),
                       rc::gen::inRange<std::uint64_t>(kMinPrice_p11,
                                                       kMaxPrice_p11 + 1),
                       rc::gen::inRange<std::uint32_t>(1, kMaxQty_p11 + 1)),
        [](const std::tuple<std::uint64_t, bool, std::uint64_t, std::uint32_t>&
               t) {
            return OrderSpec_p11{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                                 std::get<3>(t)};
        });
}

// Sum the remaining quantity of every resting order in the book, across both
// sides. Walks each side's sorted price levels (best first) and, within a
// level, its FIFO list of resting orders. Read-only and allocation-free.
template <typename Book>
std::uint64_t total_book_qty_p11(const Book& book) {
    std::uint64_t total = 0;
    for (const PriceLevel* lvl = book.best_bid(); lvl != nullptr;
         lvl = book.level_after(*lvl)) {
        for (NodeIndex n = book.level_head(*lvl); n != kNullNode;
             n = book.order_at(n).next) {
            total += book.order_at(n).remaining_qty;
        }
    }
    for (const PriceLevel* lvl = book.best_ask(); lvl != nullptr;
         lvl = book.level_after(*lvl)) {
        for (NodeIndex n = book.level_head(*lvl); n != kNullNode;
             n = book.order_at(n).next) {
            total += book.order_at(n).remaining_qty;
        }
    }
    return total;
}

}  // namespace

TEST_CASE(
    "Property 11: incoming quantity == traded quantity + rested remainder, and "
    "matching conserves total quantity",
    "[match][property][quantity-conservation]") {
    const bool ok = rc::check(
        "incoming.quantity == sum(trade qty) + rested remainder; resting "
        "reduction == traded",
        [] {
            // Build an arbitrary, internally consistent book by processing a
            // generated sequence of NewOrders. The count stays well below the
            // book's capacity so every remainder rests successfully (no pool
            // exhaustion that would silently drop quantity and break
            // conservation through a capacity limit rather than matching).
            const std::vector<OrderSpec_p11> book_orders =
                *rc::gen::resize(
                    20, rc::gen::container<std::vector<OrderSpec_p11>>(
                            gen_order_spec_p11()));

            const bool incoming_is_buy = *rc::gen::arbitrary<bool>();
            const std::uint64_t incoming_price =
                *rc::gen::inRange<std::uint64_t>(kMinPrice_p11 - 2,
                                                 kMaxPrice_p11 + 3);
            const std::uint32_t incoming_qty =
                *rc::gen::inRange<std::uint32_t>(1, 401);  // [1, 400]

            MatchingEngine<256, 256> engine;

            // Process the book-building orders. They may match against one
            // another as they arrive, leaving the book in a consistent
            // (non-crossing) state; their trades are irrelevant to this
            // property, so they are discarded.
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

            // Snapshot the total resting quantity before the incoming order.
            const std::uint64_t qty_before = total_book_qty_p11(engine.book());

            // Process one final incoming order, summing every emitted trade
            // quantity. Its identifier is distinct from every book-building
            // identifier so its rested remainder can be isolated by id.
            constexpr std::uint64_t kIncomingId = 1'000'000;
            NewOrder incoming;
            incoming.order_id = kIncomingId;
            incoming.side = incoming_is_buy ? Side::Buy : Side::Sell;
            incoming.price_ticks = incoming_price;
            incoming.quantity = incoming_qty;
            incoming.seq = seq;

            std::uint64_t traded_total = 0;
            engine.process_new_order(incoming, [&](const Trade& t) {
                traded_total += t.quantity;
            });

            const auto& book = engine.book();

            // Quantity of the incoming order's remainder that rested in the
            // book (0 if it was fully filled, so its id is absent).
            const NodeIndex inc_node = book.find_order(kIncomingId);
            const std::uint64_t rested_remainder =
                (inc_node == kNullNode)
                    ? 0
                    : static_cast<std::uint64_t>(
                          book.order_at(inc_node).remaining_qty);

            // The incoming quantity is split exactly between what traded and
            // what rested - nothing lost, nothing created.
            RC_ASSERT(traded_total + rested_remainder ==
                      static_cast<std::uint64_t>(incoming_qty));

            // System conservation: the counterparty side's resting quantity
            // dropped by exactly the traded amount. resting_after is the total
            // book quantity minus the incoming order's own rested remainder.
            const std::uint64_t qty_after = total_book_qty_p11(book);
            const std::uint64_t resting_after = qty_after - rested_remainder;
            RC_ASSERT(resting_after <= qty_before);
            RC_ASSERT(qty_before - resting_after == traded_total);
        });
    CHECK(ok);
}
