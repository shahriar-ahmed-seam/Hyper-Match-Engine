// Unit tests for the Order_Book integrity invariant checks and the atomic
// integrity guard.
//
// These verify the building blocks of the integrity guard:
//   * check_price_ordering accepts a sorted, un-crossed book and rejects a
//     crossed one.
//   * check_positive_quantity accepts a book whose resting orders all have
//     positive quantity and rejects one holding a zero-quantity order.
//   * check_quantity_conservation is exact incoming == traded + rested
//     arithmetic.
//   * IntegrityGuard::commit is atomic: a consistency-preserving operation is
//     committed, while an operation that would break price ordering, positive
//     quantity, or quantity conservation is rolled back to the exact
//     pre-operation state and reports the violated invariant by name.
//
// The dedicated fault-injection scenario through the engine path lives in a
// separate test; these tests target the guard's check functions and
// snapshot/validate/restore mechanics directly.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/integrity_guard.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::RejectReason;
using hme::Side;
using hme::engine::check_positive_quantity;
using hme::engine::check_price_ordering;
using hme::engine::check_quantity_conservation;
using hme::engine::IntegrityError;
using hme::engine::IntegrityGuard;
using hme::engine::Invariant;
using hme::engine::invariant_name;
using hme::engine::kNullNode;
using hme::engine::make_integrity_reject;
using hme::engine::OpOutcome;
using hme::engine::OrderBook;

namespace {

using Book = OrderBook<64, 64>;
using Guard = IntegrityGuard<64, 64>;

// Snapshot a book's resting state as (side, price, id, qty) tuples in best-first
// order, so two book states can be compared for exact equality after a restore.
struct Snapshot {
    Side side;
    std::uint64_t price;
    std::uint64_t id;
    std::uint32_t qty;
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

std::vector<Snapshot> snapshot_of(const Book& book) {
    std::vector<Snapshot> out;
    for (const Side side : {Side::Buy, Side::Sell}) {
        const auto* start = (side == Side::Buy) ? book.best_bid() : book.best_ask();
        for (const auto* lvl = start; lvl != nullptr; lvl = book.level_after(*lvl)) {
            for (auto node = book.level_head(*lvl); node != kNullNode;
                 node = book.order_at(node).next) {
                const auto& ro = book.order_at(node);
                out.push_back({ro.side, ro.price_ticks, ro.order_id, ro.remaining_qty});
            }
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// invariant_name / make_integrity_reject
// ---------------------------------------------------------------------------

TEST_CASE("invariant_name returns a stable name per invariant", "[integrity][name]") {
    CHECK(std::string(invariant_name(Invariant::PriceOrdering)) == "price-ordering");
    CHECK(std::string(invariant_name(Invariant::PositiveQuantity)) ==
          "positive-quantity");
    CHECK(std::string(invariant_name(Invariant::QuantityConservation)) ==
          "quantity-conservation");
}

TEST_CASE("make_integrity_reject reports an IntegrityViolation", "[integrity][reject]") {
    const auto reject = make_integrity_reject(42);
    CHECK(reject.order_id == 42);
    CHECK(reject.reason == RejectReason::IntegrityViolation);
}

// ---------------------------------------------------------------------------
// check_price_ordering
// ---------------------------------------------------------------------------

TEST_CASE("Price ordering holds for an empty book", "[integrity][price]") {
    Book book;
    CHECK(check_price_ordering(book));
}

TEST_CASE("Price ordering holds for a sorted, un-crossed book", "[integrity][price]") {
    Book book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Buy, 99, 2, 5));
    REQUIRE(book.insert_order(3, Side::Sell, 101, 3, 5));
    REQUIRE(book.insert_order(4, Side::Sell, 102, 4, 5));
    CHECK(check_price_ordering(book));  // best_bid 100 < best_ask 101.
}

TEST_CASE("Price ordering fails for a crossed book", "[integrity][price]") {
    Book book;
    // A resting buy above a resting sell crosses the book (best_bid >= best_ask).
    REQUIRE(book.insert_order(1, Side::Buy, 110, 1, 5));
    REQUIRE(book.insert_order(2, Side::Sell, 100, 2, 5));
    CHECK_FALSE(check_price_ordering(book));
}

TEST_CASE("Price ordering fails when best_bid equals best_ask", "[integrity][price]") {
    Book book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Sell, 100, 2, 5));
    CHECK_FALSE(check_price_ordering(book));  // must be strictly less.
}

// ---------------------------------------------------------------------------
// check_positive_quantity
// ---------------------------------------------------------------------------

TEST_CASE("Positive quantity holds when every resting order is non-empty",
          "[integrity][qty]") {
    Book book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Sell, 110, 2, 3));
    CHECK(check_positive_quantity(book));
}

TEST_CASE("Positive quantity fails when a resting order has zero quantity",
          "[integrity][qty]") {
    Book book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Buy, 99, 2, 5));
    // Corrupt one resting order to a zero remaining quantity.
    book.order_at(book.find_order(2)).remaining_qty = 0;
    CHECK_FALSE(check_positive_quantity(book));
}

// ---------------------------------------------------------------------------
// check_quantity_conservation
// ---------------------------------------------------------------------------

TEST_CASE("Quantity conservation is exact incoming == traded + rested",
          "[integrity][conservation]") {
    CHECK(check_quantity_conservation(/*incoming=*/10, /*traded=*/4, /*rested=*/6));
    CHECK(check_quantity_conservation(7, 7, 0));   // fully matched.
    CHECK(check_quantity_conservation(7, 0, 7));   // fully rested.
    CHECK_FALSE(check_quantity_conservation(10, 4, 5));  // a unit lost.
    CHECK_FALSE(check_quantity_conservation(10, 4, 7));  // a unit created.
}

// ---------------------------------------------------------------------------
// IntegrityGuard::commit - the atomic guard
// ---------------------------------------------------------------------------

TEST_CASE("Guard commits a consistency-preserving operation", "[integrity][guard]") {
    Book book;
    REQUIRE(book.insert_order(1, Side::Sell, 110, 1, 5));
    Guard guard;
    std::vector<IntegrityError> errors;

    const bool committed = guard.commit(
        book,
        [&]() {
            // Insert a non-crossing buy and report it as fully rested.
            book.insert_order(2, Side::Buy, 100, 2, 4);
            return OpOutcome::conserving(/*incoming=*/4, /*traded=*/0, /*rested=*/4);
        },
        [&](const IntegrityError& e) { errors.push_back(e); });

    CHECK(committed);
    CHECK(errors.empty());
    CHECK(book.order_count() == 2);  // the buy stayed in the book.
    CHECK(book.find_order(2) != kNullNode);
}

TEST_CASE("Guard rolls back a price-ordering violation and names it",
          "[integrity][guard]") {
    Book book;
    REQUIRE(book.insert_order(1, Side::Sell, 100, 1, 5));
    const auto before = snapshot_of(book);

    Guard guard;
    std::vector<IntegrityError> errors;
    const bool committed = guard.commit(
        book,
        [&]() {
            // A buggy operation rests a buy above the resting ask -> crossed book.
            book.insert_order(2, Side::Buy, 110, 2, 5);
            return OpOutcome::no_conservation();
        },
        [&](const IntegrityError& e) { errors.push_back(e); });

    CHECK_FALSE(committed);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].invariant == Invariant::PriceOrdering);
    CHECK(std::string(errors[0].name) == "price-ordering");
    // Book restored to its exact pre-operation state.
    CHECK(snapshot_of(book) == before);
    CHECK(book.find_order(2) == kNullNode);
}

TEST_CASE("Guard rolls back a positive-quantity violation and names it",
          "[integrity][guard]") {
    Book book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    const auto before = snapshot_of(book);

    Guard guard;
    std::vector<IntegrityError> errors;
    const bool committed = guard.commit(
        book,
        [&]() {
            // A buggy operation rests a zero-quantity order.
            book.insert_order(2, Side::Buy, 99, 2, 5);
            book.order_at(book.find_order(2)).remaining_qty = 0;
            return OpOutcome::no_conservation();
        },
        [&](const IntegrityError& e) { errors.push_back(e); });

    CHECK_FALSE(committed);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].invariant == Invariant::PositiveQuantity);
    CHECK(snapshot_of(book) == before);
}

TEST_CASE("Guard rolls back a quantity-conservation violation and names it",
          "[integrity][guard]") {
    Book book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    const auto before = snapshot_of(book);

    Guard guard;
    std::vector<IntegrityError> errors;
    const bool committed = guard.commit(
        book,
        [&]() {
            // The book stays consistent, but the reported quantities do not add
            // up: incoming 10 != traded 4 + rested 5.
            book.insert_order(2, Side::Sell, 110, 2, 5);
            return OpOutcome::conserving(/*incoming=*/10, /*traded=*/4, /*rested=*/5);
        },
        [&](const IntegrityError& e) { errors.push_back(e); });

    CHECK_FALSE(committed);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].invariant == Invariant::QuantityConservation);
    CHECK(snapshot_of(book) == before);
}

TEST_CASE("Guard snapshot and restore round-trips an arbitrary book",
          "[integrity][guard]") {
    Book book;
    REQUIRE(book.insert_order(1, Side::Buy, 100, 1, 5));
    REQUIRE(book.insert_order(2, Side::Buy, 99, 2, 7));
    REQUIRE(book.insert_order(3, Side::Sell, 110, 3, 9));
    const auto before = snapshot_of(book);

    Guard guard;
    guard.snapshot(book);

    // Mutate the book heavily after snapshotting.
    book.remove_order(book.find_order(1));
    REQUIRE(book.insert_order(4, Side::Sell, 120, 4, 1));
    CHECK(snapshot_of(book) != before);

    guard.restore(book);
    CHECK(snapshot_of(book) == before);
}
