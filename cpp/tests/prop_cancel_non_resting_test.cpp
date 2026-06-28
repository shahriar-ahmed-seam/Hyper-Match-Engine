// Cancellation of a non-resting order is rejected without effect. For any
// Cancel_Request referencing an identifier that is not present in the
// Order_Book - whether never seen, already fully filled, or already cancelled -
// the Matching_Engine leaves the Order_Book unchanged and emits exactly one
// cancellation rejection containing the identifier and the appropriate reason.
//
// Engine behavior this test asserts against: a non-resting cancel is always
// rejected with exactly one Reject and leaves the book completely unchanged.
// The engine carries a bounded retired-id tracker recording ids that have left
// the book (fully filled during matching, or cancelled), so it distinguishes
// the reason: an id that was once resting but is no longer reports
// RejectReason::NoLongerResting, while an id the engine has never seen reports
// RejectReason::OrderNotFound. The generator below covers all three sub-cases -
// "never seen" (OrderNotFound), "already filled" and "already cancelled" (both
// NoLongerResting).
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p16` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstdint>
#include <tuple>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/matching_engine.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::Ack;
using hme::CancelOrder;
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
struct RestingSpec_p16 {
    Side side = Side::Buy;
    std::uint64_t price_ticks = 0;
    std::uint32_t quantity = 0;
};

// A captured resting order: every field that defines the book's observable
// state. Comparing the ordered vector across the whole book gives a faithful
// "byte-for-byte unchanged" check.
struct OrderSnap_p16 {
    Side side = Side::Buy;
    std::uint64_t price_ticks = 0;
    std::uint64_t order_id = 0;
    std::uint64_t seq = 0;
    std::uint32_t remaining_qty = 0;

    friend bool operator==(const OrderSnap_p16&, const OrderSnap_p16&) = default;
};

// Walk the book in canonical best-first order (all bids best-first, then all
// asks best-first; within each level by (seq, order_id)) and record every
// resting order. The traversal order is fully determined by the book's
// structure, so two snapshots of an identical book are identical vectors.
std::vector<OrderSnap_p16> snapshot_p16(const Book& book) {
    std::vector<OrderSnap_p16> out;
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
// reaches a non-trivial, valid state. order_id / seq are assigned by index
// after generation so they are unique and monotonic.
rc::Gen<RestingSpec_p16> gen_resting_spec_p16() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<bool>(),
                       rc::gen::inRange<std::uint64_t>(0, 10),
                       rc::gen::inRange<std::uint32_t>(1, 51)),
        [](const std::tuple<bool, std::uint64_t, std::uint32_t>& t) {
            const bool is_buy = std::get<0>(t);
            const std::uint64_t price =
                is_buy ? (90 + std::get<1>(t)) : (110 + std::get<1>(t));
            RestingSpec_p16 s;
            s.side = is_buy ? Side::Buy : Side::Sell;
            s.price_ticks = price;
            s.quantity = std::get<2>(t);
            return s;
        });
}

// The way the cancel target comes to be non-resting. NeverSeen must yield
// OrderNotFound; AlreadyFilled and AlreadyCancelled were once resting and so
// must yield NoLongerResting.
enum class NonResting_p16 {
    NeverSeen,        // an id never submitted to the engine.
    AlreadyFilled,    // an id that rested then was fully filled.
    AlreadyCancelled  // an id that rested then was cancelled.
};

rc::Gen<NonResting_p16> gen_non_resting_kind_p16() {
    return rc::gen::element(NonResting_p16::NeverSeen,
                            NonResting_p16::AlreadyFilled,
                            NonResting_p16::AlreadyCancelled);
}

}  // namespace

TEST_CASE(
    "Property 16: cancelling a non-resting order is rejected without effect",
    "[cancel][reject][property][non-resting]") {
    const bool ok = rc::check(
        "non-resting cancel -> one Reject (appropriate reason), no Ack, book unchanged",
        [] {
            // Build an arbitrary pre-existing book. The resting-order count is
            // capped well below the book's capacity (64) so every generated
            // order rests successfully.
            const std::vector<RestingSpec_p16> resting =
                *rc::gen::resize(
                    20, rc::gen::container<std::vector<RestingSpec_p16>>(
                            gen_resting_spec_p16()));

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
                engine.process_new_order(o, [](const Trade&) {});
            }

            // Choose how the cancel target becomes non-resting and arrange for
            // it. `target_id` is the identifier we will later try to cancel.
            const NonResting_p16 kind = *gen_non_resting_kind_p16();
            std::uint64_t target_id = 0;
            switch (kind) {
                case NonResting_p16::NeverSeen: {
                    // An id far outside the range of any assigned resting id;
                    // it has never been seen by the engine.
                    target_id = 1'000'000 + *rc::gen::inRange<std::uint64_t>(
                                                 0, 1'000'000);
                    break;
                }
                case NonResting_p16::AlreadyFilled: {
                    // Rest a fresh order, then fully fill it with a crossing
                    // order so its id is no longer resting.
                    target_id = next_id++;
                    NewOrder maker;
                    maker.order_id = target_id;
                    maker.side = Side::Sell;
                    maker.price_ticks = 100;  // between bid (<=99) and ask (>=110) bands.
                    maker.quantity = 10;
                    maker.seq = next_seq++;
                    engine.process_new_order(maker, [](const Trade&) {});

                    NewOrder taker;
                    taker.order_id = next_id++;
                    taker.side = Side::Buy;
                    taker.price_ticks = 100;  // crosses and fully fills the maker.
                    taker.quantity = 10;
                    taker.seq = next_seq++;
                    engine.process_new_order(taker, [](const Trade&) {});
                    break;
                }
                case NonResting_p16::AlreadyCancelled: {
                    // Rest a fresh order, then cancel it so its id is no longer
                    // resting.
                    target_id = next_id++;
                    NewOrder maker;
                    maker.order_id = target_id;
                    maker.side = Side::Buy;
                    maker.price_ticks = 95;  // within the bid band, rests cleanly.
                    maker.quantity = 7;
                    maker.seq = next_seq++;
                    engine.process_new_order(maker, [](const Trade&) {});

                    CancelOrder first;
                    first.order_id = target_id;
                    engine.process_cancel_order(
                        first, [](const Ack&) {}, [](const Reject&) {});
                    break;
                }
            }

            // The target must not be resting at this point, regardless of kind.
            RC_ASSERT(engine.book().find_order(target_id) == kNullNode);

            // Snapshot the book just before the cancel under test.
            const std::vector<OrderSnap_p16> before = snapshot_p16(engine.book());
            const auto order_count_before = engine.book().order_count();
            const auto level_count_before = engine.book().level_count();

            // Cancel the non-resting identifier.
            CancelOrder cancel;
            cancel.order_id = target_id;
            std::vector<Ack> acks;
            std::vector<Reject> rejects;
            const bool removed = engine.process_cancel_order(
                cancel, [&](const Ack& a) { acks.push_back(a); },
                [&](const Reject& r) { rejects.push_back(r); });

            // No removal occurred.
            RC_ASSERT(!removed);

            // Exactly one Reject, carrying the target id and the appropriate
            // reason; no Ack at all. A never-seen id reports OrderNotFound; an
            // id that was once resting (filled or cancelled) reports
            // NoLongerResting.
            const RejectReason expected_reason =
                (kind == NonResting_p16::NeverSeen)
                    ? RejectReason::OrderNotFound
                    : RejectReason::NoLongerResting;
            RC_ASSERT(acks.empty());
            RC_ASSERT(rejects.size() == 1u);
            RC_ASSERT(rejects[0].order_id == target_id);
            RC_ASSERT(rejects[0].reason == expected_reason);

            // The book is byte-for-byte unchanged: same ordered resting orders
            // and same counts (rejected without effect).
            const std::vector<OrderSnap_p16> after = snapshot_p16(engine.book());
            RC_ASSERT(after == before);
            RC_ASSERT(engine.book().order_count() == order_count_before);
            RC_ASSERT(engine.book().level_count() == level_count_before);
        });
    CHECK(ok);
}
