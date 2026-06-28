// Resting-order positive-quantity invariant. For any sequence of processed
// messages, after each message every Resting_Order in the Order_Book has
// remaining quantity strictly greater than zero (equivalently, any order whose
// remaining quantity reaches zero has been removed).
//
// It generates an arbitrary stream of valid operations - a mix of NewOrders and
// CancelOrders - feeds them to the MatchingEngine one at a time, and after EACH
// operation asserts that the Order_Book satisfies the positive-quantity
// invariant via hme::engine::check_positive_quantity: every resting order has
// strictly positive remaining quantity, so a fully filled order (whose remaining
// quantity reached zero) must have been removed rather than left at zero. The
// invariant must hold not only at the end of the stream but after every
// individual operation.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p14` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstdint>
#include <tuple>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/integrity_guard.hpp"
#include "hme/matching_engine.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::CancelOrder;
using hme::NewOrder;
using hme::Side;
using hme::Trade;
using hme::engine::check_positive_quantity;
using hme::engine::MatchingEngine;

namespace {

// A single generated operation in the stream: either a NewOrder placement or a
// CancelOrder. A discriminator (`is_new`) selects which fields are meaningful.
struct OpSpec_p14 {
    bool is_new = true;             // true -> NewOrder; false -> CancelOrder.
    std::uint64_t order_id = 0;     // identifier for both NewOrder and Cancel.
    bool is_buy = true;             // NewOrder side.
    std::uint64_t price_ticks = 0;  // NewOrder limit price (valid range).
    std::uint32_t quantity = 0;     // NewOrder quantity (valid range).
};

// Small, overlapping domains chosen so that operations interact heavily: orders
// frequently land on the same handful of price levels (driving crossing and
// matching), buys and sells frequently cross so resting orders are partially
// and fully filled, and order_ids repeat enough that CancelOrders frequently
// reference a resting order. This stresses every code path that can leave (or
// must avoid leaving) a zero-quantity resting order in the book.
constexpr std::uint64_t kMinPrice_p14 = 95;
constexpr std::uint64_t kMaxPrice_p14 = 110;  // inclusive
constexpr std::uint32_t kMaxQty_p14 = 40;
constexpr std::uint64_t kMaxId_p14 = 30;  // ids in [1, 30]

// Generator for a single operation. ~1 in 3 operations is a CancelOrder; the
// rest are NewOrders spanning both sides, the full local price band, and valid
// quantities. All generated NewOrders are within the engine's accepted price
// and quantity ranges, so they are genuine "valid operations".
rc::Gen<OpSpec_p14> gen_op_spec_p14() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange<int>(0, 3),  // 0 -> cancel, 1/2 -> new.
                       rc::gen::inRange<std::uint64_t>(1, kMaxId_p14 + 1),
                       rc::gen::arbitrary<bool>(),
                       rc::gen::inRange<std::uint64_t>(kMinPrice_p14,
                                                       kMaxPrice_p14 + 1),
                       rc::gen::inRange<std::uint32_t>(1, kMaxQty_p14 + 1)),
        [](const std::tuple<int, std::uint64_t, bool, std::uint64_t,
                            std::uint32_t>& t) {
            OpSpec_p14 op;
            op.is_new = std::get<0>(t) != 0;
            op.order_id = std::get<1>(t);
            op.is_buy = std::get<2>(t);
            op.price_ticks = std::get<3>(t);
            op.quantity = std::get<4>(t);
            return op;
        });
}

}  // namespace

TEST_CASE(
    "Property 14: every resting order has strictly positive quantity after "
    "every operation",
    "[match][property][positive-quantity][integrity]") {
    const bool ok = rc::check(
        "after each processed message no resting order has zero quantity",
        [] {
            // An arbitrary stream of valid operations. The count stays well
            // below the book's capacity (256) so that every resting remainder
            // can be placed without pool exhaustion.
            const std::vector<OpSpec_p14> ops =
                *rc::gen::resize(60, rc::gen::container<std::vector<OpSpec_p14>>(
                                         gen_op_spec_p14()));

            MatchingEngine<256, 256> engine;

            // The invariant holds trivially on the empty book before any
            // operation is processed.
            RC_ASSERT(check_positive_quantity(engine.book()));

            std::uint64_t seq = 1;
            for (const auto& op : ops) {
                if (op.is_new) {
                    NewOrder o;
                    o.order_id = op.order_id;
                    o.side = op.is_buy ? Side::Buy : Side::Sell;
                    o.price_ticks = op.price_ticks;
                    o.quantity = op.quantity;
                    o.seq = seq++;
                    engine.process_new_order(o, [](const Trade&) {});
                } else {
                    CancelOrder c;
                    c.order_id = op.order_id;
                    engine.process_cancel_order(
                        c, [](const hme::Ack&) {}, [](const hme::Reject&) {});
                }

                // After EACH processed message every resting order must have
                // remaining quantity strictly greater than zero; a fully filled
                // order must have been removed, never left resting at zero.
                RC_ASSERT(check_positive_quantity(engine.book()));
            }
        });
    CHECK(ok);
}
