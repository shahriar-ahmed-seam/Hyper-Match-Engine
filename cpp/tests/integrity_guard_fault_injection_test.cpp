// Fault-injection unit test for the Order_Book integrity guard.
//
// If an operation would violate price ordering, positive quantity, or quantity
// conservation for an Order, the Matching_Engine rejects the operation, restores
// the Order_Book to its state prior to the operation, and emits an error
// indication identifying the violated integrity invariant.
//
// Where the companion integrity_guard_test.cpp exercises the guard's building
// blocks (the pure check functions and snapshot/validate/restore mechanics),
// this file is a dedicated fault-injection test: it deliberately forces a
// guarded operation to corrupt the book in each of the three ways an invariant
// can be broken, and asserts the two halves of the contract:
//
//   (a) the book is RESTORED *byte-for-byte* to its exact pre-operation image
//       (a raw std::memcmp over the whole OrderBook object, not merely a
//       logical re-comparison of the resting orders), and
//   (b) exactly one error is emitted, NAMING the violated invariant.
//
// The byte-for-byte check is meaningful because OrderBook is a flat,
// pre-allocated aggregate (inline std::array pools + scalar indices) and so is
// trivially copyable; a correct restore must reproduce every byte of pool
// storage and free-list threading, not just the orders reachable by a walk.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/integrity_guard.hpp"
#include "hme/order_book.hpp"
#include "hme/wire_protocol.hpp"

using hme::RejectReason;
using hme::Side;
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

// The byte-for-byte assertion below relies on the book being a flat,
// trivially-copyable image with no pointers or external ownership.
static_assert(std::is_trivially_copyable_v<Book>,
              "OrderBook must be trivially copyable for a byte-for-byte "
              "restore comparison");

// A raw image of the whole book object, including pool storage, free-list
// threading, and any padding - everything a faithful restore must reproduce.
using RawImage = std::array<unsigned char, sizeof(Book)>;

RawImage raw_image_of(const Book& book) noexcept {
    RawImage image{};
    std::memcpy(image.data(), &book, sizeof(Book));
    return image;
}

// Build a representative, two-sided book so a restore has real structure
// (multiple price levels per side and a multi-order level) to reproduce.
void seed_book(Book& book) {
    REQUIRE(book.insert_order(/*id=*/1, Side::Buy, /*px=*/100, /*seq=*/1, /*qty=*/5));
    REQUIRE(book.insert_order(/*id=*/2, Side::Buy, /*px=*/100, /*seq=*/2, /*qty=*/7));
    REQUIRE(book.insert_order(/*id=*/3, Side::Buy, /*px=*/99, /*seq=*/3, /*qty=*/4));
    REQUIRE(book.insert_order(/*id=*/4, Side::Sell, /*px=*/110, /*seq=*/4, /*qty=*/6));
    REQUIRE(book.insert_order(/*id=*/5, Side::Sell, /*px=*/111, /*seq=*/5, /*qty=*/8));
}

}  // namespace

// ---------------------------------------------------------------------------
// Fault injection 1: an operation that crosses the book (price-ordering).
// ---------------------------------------------------------------------------
TEST_CASE(
    "Fault injection: crossing the book is rejected, book restored byte-for-byte, "
    "price-ordering named",
    "[integrity][fault-injection][price]") {
    Book book;
    seed_book(book);
    const RawImage before = raw_image_of(book);
    const auto before_count = book.order_count();

    Guard guard;
    std::vector<IntegrityError> errors;
    const bool committed = guard.commit(
        book,
        [&]() {
            // Rest a buy at 110, at or above the best ask (110) -> crossed book.
            book.insert_order(/*id=*/99, Side::Buy, /*px=*/110, /*seq=*/6,
                              /*qty=*/3);
            return OpOutcome::no_conservation();
        },
        [&](const IntegrityError& e) { errors.push_back(e); });

    // (a) operation rejected and the book restored byte-for-byte.
    CHECK_FALSE(committed);
    CHECK(raw_image_of(book) == before);
    CHECK(book.order_count() == before_count);
    CHECK(book.find_order(99) == kNullNode);  // the faulty order is gone.

    // (b) exactly one error, naming the violated invariant.
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].invariant == Invariant::PriceOrdering);
    CHECK(std::string(errors[0].name) == "price-ordering");
    CHECK(std::string(errors[0].name) ==
          std::string(invariant_name(Invariant::PriceOrdering)));
}

// ---------------------------------------------------------------------------
// Fault injection 2: an operation that rests a zero-quantity order
// (positive-quantity).
// ---------------------------------------------------------------------------
TEST_CASE(
    "Fault injection: a zero-quantity resting order is rejected, book restored "
    "byte-for-byte, positive-quantity named",
    "[integrity][fault-injection][qty]") {
    Book book;
    seed_book(book);
    const RawImage before = raw_image_of(book);

    Guard guard;
    std::vector<IntegrityError> errors;
    const bool committed = guard.commit(
        book,
        [&]() {
            // Rest a non-crossing buy, then corrupt it to zero remaining qty.
            book.insert_order(/*id=*/99, Side::Buy, /*px=*/98, /*seq=*/6,
                              /*qty=*/5);
            book.order_at(book.find_order(99)).remaining_qty = 0;
            return OpOutcome::no_conservation();
        },
        [&](const IntegrityError& e) { errors.push_back(e); });

    CHECK_FALSE(committed);
    CHECK(raw_image_of(book) == before);
    CHECK(book.find_order(99) == kNullNode);

    REQUIRE(errors.size() == 1);
    CHECK(errors[0].invariant == Invariant::PositiveQuantity);
    CHECK(std::string(errors[0].name) == "positive-quantity");
    CHECK(std::string(errors[0].name) ==
          std::string(invariant_name(Invariant::PositiveQuantity)));
}

// ---------------------------------------------------------------------------
// Fault injection 3: an operation reporting a broken quantity balance
// (quantity-conservation). The book stays structurally consistent, but the
// reported incoming/traded/rested quantities do not add up.
// ---------------------------------------------------------------------------
TEST_CASE(
    "Fault injection: a broken quantity balance is rejected, book restored "
    "byte-for-byte, quantity-conservation named",
    "[integrity][fault-injection][conservation]") {
    Book book;
    seed_book(book);
    const RawImage before = raw_image_of(book);

    Guard guard;
    std::vector<IntegrityError> errors;
    const bool committed = guard.commit(
        book,
        [&]() {
            // A consistent mutation, but the reported balance loses a unit:
            // incoming 10 != traded 4 + rested 5.
            book.insert_order(/*id=*/99, Side::Sell, /*px=*/112, /*seq=*/6,
                              /*qty=*/5);
            return OpOutcome::conserving(/*incoming=*/10, /*traded=*/4,
                                         /*rested=*/5);
        },
        [&](const IntegrityError& e) { errors.push_back(e); });

    CHECK_FALSE(committed);
    CHECK(raw_image_of(book) == before);
    CHECK(book.find_order(99) == kNullNode);

    REQUIRE(errors.size() == 1);
    CHECK(errors[0].invariant == Invariant::QuantityConservation);
    CHECK(std::string(errors[0].name) == "quantity-conservation");
    CHECK(std::string(errors[0].name) ==
          std::string(invariant_name(Invariant::QuantityConservation)));
}

// ---------------------------------------------------------------------------
// The emitted violation maps to an IntegrityViolation Reject for the offending
// incoming order, closing the loop on "emit an error indication".
// ---------------------------------------------------------------------------
TEST_CASE(
    "Fault injection: a violation converts to an IntegrityViolation reject for "
    "the offending order",
    "[integrity][fault-injection][reject]") {
    Book book;
    seed_book(book);

    Guard guard;
    std::vector<IntegrityError> errors;
    constexpr std::uint64_t kOffendingId = 99;
    const bool committed = guard.commit(
        book,
        [&]() {
            book.insert_order(kOffendingId, Side::Buy, /*px=*/110, /*seq=*/6,
                              /*qty=*/3);
            return OpOutcome::no_conservation();
        },
        [&](const IntegrityError& e) { errors.push_back(e); });

    REQUIRE_FALSE(committed);
    REQUIRE(errors.size() == 1);

    const hme::Reject reject = make_integrity_reject(kOffendingId);
    CHECK(reject.order_id == kOffendingId);
    CHECK(reject.reason == RejectReason::IntegrityViolation);
}
