// Invalid order rejection preserves the book. For any incoming Order whose
// quantity is not an integer in 1-1,000,000 or whose price is outside
// 0.01-999,999,999.99 (ticks [kMinPriceTicks, kMaxPriceTicks]), the
// Matching_Engine emits a rejection event indicating the validation failure and
// leaves the Order_Book identical to its state before the Order.
//
// It builds an arbitrary pre-existing book state from a generated set of valid
// resting orders, snapshots the book, submits an arbitrary *invalid* incoming
// order through the validating two-sink process_new_order overload, and asserts:
//
//   * exactly one Reject is emitted (and no other), carrying the incoming
//     order's identifier and the correct RejectReason (InvalidQuantity when the
//     quantity is out of range - checked first by the engine - otherwise
//     InvalidPrice);
//   * no Trade events are emitted; and
//   * the Order_Book is byte-for-byte unchanged - the ordered sequence of
//     resting orders (side, price, id, seq, remaining quantity) across both
//     sides, and the order/level counts, are identical before and after.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p12` so they do not clash (ODR) with generators in other property-test TUs.

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
using hme::Reject;
using hme::RejectReason;
using hme::Side;
using hme::Trade;
using hme::engine::kNullNode;
using hme::engine::MatchingEngine;
using hme::engine::NodeIndex;
using hme::engine::PriceLevel;

namespace {

using Engine = MatchingEngine<64, 64>;
using Book = Engine::Book;

// A valid resting-order specification used to populate the pre-existing book.
struct RestingSpec_p12 {
    std::uint64_t order_id = 0;
    Side side = Side::Buy;
    std::uint64_t price_ticks = 0;
    std::uint32_t quantity = 0;
};

// A captured resting order: every field that defines the book's observable
// state. Comparing the ordered vector of these across the whole book gives a
// faithful "byte-for-byte unchanged" check.
struct OrderSnap_p12 {
    Side side = Side::Buy;
    std::uint64_t price_ticks = 0;
    std::uint64_t order_id = 0;
    std::uint64_t seq = 0;
    std::uint32_t remaining_qty = 0;

    friend bool operator==(const OrderSnap_p12&, const OrderSnap_p12&) = default;
};

// Walk the book in canonical best-first order (all bids best-first, then all
// asks best-first; within each level by (seq, order_id)) and record every
// resting order. The traversal order is fully determined by the book's
// structure, so two snapshots of an identical book are identical vectors.
std::vector<OrderSnap_p12> snapshot_p12(const Book& book) {
    std::vector<OrderSnap_p12> out;
    for (const PriceLevel* lvl = book.best_bid(); lvl != nullptr;
         lvl = book.level_after(*lvl)) {
        for (NodeIndex n = book.level_head(*lvl); n != kNullNode;
             n = book.order_at(n).next) {
            const auto& ro = book.order_at(n);
            out.push_back({ro.side, ro.price_ticks, ro.order_id, ro.seq,
                           ro.remaining_qty});
        }
    }
    for (const PriceLevel* lvl = book.best_ask(); lvl != nullptr;
         lvl = book.level_after(*lvl)) {
        for (NodeIndex n = book.level_head(*lvl); n != kNullNode;
             n = book.order_at(n).next) {
            const auto& ro = book.order_at(n);
            out.push_back({ro.side, ro.price_ticks, ro.order_id, ro.seq,
                           ro.remaining_qty});
        }
    }
    return out;
}

// Generator for one valid resting order. Buys are priced strictly below sells
// (bids in [90, 99], asks in [110, 119]) so the two sides never cross while the
// book is being populated; every generated order therefore rests and the book
// reaches a non-trivial, valid state. Quantities stay well within the engine's
// permitted range. order_id / seq are assigned by index after generation so
// they are unique and monotonic.
rc::Gen<RestingSpec_p12> gen_resting_spec_p12() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<bool>(),
                       rc::gen::inRange<std::uint64_t>(0, 10),
                       rc::gen::inRange<std::uint32_t>(1, 51)),
        [](const std::tuple<bool, std::uint64_t, std::uint32_t>& t) {
            const bool is_buy = std::get<0>(t);
            const std::uint64_t price =
                is_buy ? (90 + std::get<1>(t)) : (110 + std::get<1>(t));
            RestingSpec_p12 s;
            s.side = is_buy ? Side::Buy : Side::Sell;
            s.price_ticks = price;
            s.quantity = std::get<2>(t);
            return s;
        });
}

// How an invalid incoming order violates the engine's matching domain.
enum class Violation_p12 {
    QtyZero,        // quantity 0           -> InvalidQuantity
    QtyTooLarge,    // quantity > 1,000,000 -> InvalidQuantity
    PriceZero,      // price ticks 0        -> InvalidPrice (quantity valid)
    PriceTooLarge,  // price > max ticks    -> InvalidPrice (quantity valid)
};

// An invalid incoming order plus the RejectReason the engine must report. The
// engine checks quantity before price, so any quantity violation yields
// InvalidQuantity regardless of price; price violations therefore pair with a
// valid quantity.
struct InvalidOrder_p12 {
    std::uint64_t price_ticks = 0;
    std::uint32_t quantity = 0;
    RejectReason expected_reason = RejectReason::InvalidQuantity;
};

rc::Gen<InvalidOrder_p12> gen_invalid_order_p12() {
    return rc::gen::mapcat(
        rc::gen::element(Violation_p12::QtyZero, Violation_p12::QtyTooLarge,
                         Violation_p12::PriceZero, Violation_p12::PriceTooLarge),
        [](Violation_p12 v) -> rc::Gen<InvalidOrder_p12> {
            switch (v) {
                case Violation_p12::QtyZero:
                    // Valid price, zero quantity.
                    return rc::gen::map(
                        rc::gen::inRange<std::uint64_t>(
                            hme::limits::kMinPriceTicks,
                            hme::limits::kMaxPriceTicks + 1),
                        [](std::uint64_t price) {
                            return InvalidOrder_p12{price, 0,
                                                    RejectReason::InvalidQuantity};
                        });
                case Violation_p12::QtyTooLarge:
                    // Valid price, quantity above the engine maximum.
                    return rc::gen::map(
                        rc::gen::tuple(
                            rc::gen::inRange<std::uint64_t>(
                                hme::limits::kMinPriceTicks,
                                hme::limits::kMaxPriceTicks + 1),
                            rc::gen::inRange<std::uint32_t>(
                                hme::limits::kMaxEngineQuantity + 1,
                                hme::limits::kMaxEngineQuantity * 2)),
                        [](const std::tuple<std::uint64_t, std::uint32_t>& t) {
                            return InvalidOrder_p12{std::get<0>(t), std::get<1>(t),
                                                    RejectReason::InvalidQuantity};
                        });
                case Violation_p12::PriceZero:
                    // Valid quantity, zero price ticks.
                    return rc::gen::map(
                        rc::gen::inRange<std::uint32_t>(
                            hme::limits::kMinEngineQuantity,
                            hme::limits::kMaxEngineQuantity + 1),
                        [](std::uint32_t qty) {
                            return InvalidOrder_p12{0, qty,
                                                    RejectReason::InvalidPrice};
                        });
                case Violation_p12::PriceTooLarge:
                default:
                    // Valid quantity, price above the maximum tick.
                    return rc::gen::map(
                        rc::gen::tuple(
                            rc::gen::inRange<std::uint64_t>(
                                hme::limits::kMaxPriceTicks + 1,
                                hme::limits::kMaxPriceTicks + 1'000'000),
                            rc::gen::inRange<std::uint32_t>(
                                hme::limits::kMinEngineQuantity,
                                hme::limits::kMaxEngineQuantity + 1)),
                        [](const std::tuple<std::uint64_t, std::uint32_t>& t) {
                            return InvalidOrder_p12{std::get<0>(t), std::get<1>(t),
                                                    RejectReason::InvalidPrice};
                        });
            }
        });
}

}  // namespace

TEST_CASE("Property 12: an invalid order is rejected and leaves the book unchanged",
          "[match][validate][property][preserve]") {
    const bool ok = rc::check(
        "invalid order -> one correct Reject, no trades, book byte-identical",
        [] {
            // Build an arbitrary pre-existing book. The resting-order count is
            // capped well below the book's capacity (64) so every generated
            // order rests successfully.
            const std::vector<RestingSpec_p12> resting =
                *rc::gen::resize(
                    20, rc::gen::container<std::vector<RestingSpec_p12>>(
                            gen_resting_spec_p12()));

            Engine engine;
            std::uint64_t next_id = 1;
            std::uint64_t next_seq = 1;
            for (const auto& s : resting) {
                NewOrder o;
                o.order_id = next_id++;
                o.side = s.side;
                o.price_ticks = s.price_ticks;
                o.quantity = s.quantity;
                o.seq = next_seq++;
                // Single-sink overload: these are valid, non-crossing orders.
                engine.process_new_order(o, [](const Trade&) {});
            }

            // Snapshot the book and engine state before the invalid order.
            const std::vector<OrderSnap_p12> before = snapshot_p12(engine.book());
            const auto order_count_before = engine.book().order_count();
            const auto level_count_before = engine.book().level_count();
            const std::uint64_t exec_seq_before = engine.next_exec_seq();

            // Submit an arbitrary invalid incoming order through the validating
            // two-sink overload.
            const InvalidOrder_p12 bad = *gen_invalid_order_p12();
            NewOrder incoming;
            incoming.order_id = 1'000'000;  // distinct from any resting id.
            incoming.side = *rc::gen::element(Side::Buy, Side::Sell);
            incoming.price_ticks = bad.price_ticks;
            incoming.quantity = bad.quantity;
            incoming.seq = next_seq;

            std::vector<Trade> trades;
            std::vector<Reject> rejects;
            const bool accepted = engine.process_new_order(
                incoming, [&](const Trade& t) { trades.push_back(t); },
                [&](const Reject& r) { rejects.push_back(r); });

            // The order must be rejected, not accepted.
            RC_ASSERT(!accepted);

            // Exactly one Reject, carrying the incoming id and the correct
            // reason; no Trades at all.
            RC_ASSERT(rejects.size() == 1u);
            RC_ASSERT(rejects[0].order_id == incoming.order_id);
            RC_ASSERT(rejects[0].reason == bad.expected_reason);
            RC_ASSERT(trades.empty());

            // The book is byte-for-byte unchanged: same ordered resting orders,
            // same counts, no execution sequence consumed.
            const std::vector<OrderSnap_p12> after = snapshot_p12(engine.book());
            RC_ASSERT(after == before);
            RC_ASSERT(engine.book().order_count() == order_count_before);
            RC_ASSERT(engine.book().level_count() == level_count_before);
            RC_ASSERT(engine.next_exec_seq() == exec_seq_before);
        });
    CHECK(ok);
}
