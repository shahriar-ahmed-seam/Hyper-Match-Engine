// Unit tests for the Matching_Engine startup memory reservation.
//
// These exercise the successful-startup contract of
// hme::engine::EngineProcessor::initialize:
//   * A satisfiable configuration reserves all Ring_Buffer and Order_Book
//     working memory, clears any startup error, and brings the processor to its
//     operational state with a clean, empty book.
//   * Once operational, the processor processes submitted orders normally.
//
// The startup-FAILURE path (a reservation that cannot be satisfied -> abort,
// insufficient-memory error, non-operational, no Order processed) is covered by
// a separate unit test; this file only demonstrates the success path.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "hme/engine_processor.hpp"
#include "hme/wire_protocol.hpp"

using hme::NewOrder;
using hme::Side;
using hme::engine::EngineProcessor;
using hme::engine::StartupError;

namespace {

using Processor = EngineProcessor<64, 64, 64, 256>;

NewOrder make_order(std::uint64_t id, Side side, std::uint64_t price,
                    std::uint32_t qty, std::uint64_t seq) {
    NewOrder o;
    o.order_id = id;
    o.side = side;
    o.price_ticks = price;
    o.quantity = qty;
    o.seq = seq;
    return o;
}

}  // namespace

TEST_CASE("initialize with the default config reserves memory and goes operational",
          "[startup]") {
    Processor proc;
    REQUIRE(proc.initialize());
    CHECK(proc.operational());
    CHECK(proc.startup_error() == StartupError::None);
    CHECK(proc.engine().book().empty());
}

TEST_CASE("initialize with capacities within the reserved fixed capacities succeeds",
          "[startup]") {
    Processor proc;
    Processor::EngineConfig cfg;
    cfg.max_orders = 32;          // <= 64 reserved
    cfg.max_price_levels = 16;    // <= 64 reserved
    cfg.ingress_capacity = 64;    // == 64 reserved (boundary)
    cfg.egress_capacity = 128;    // <= 256 reserved

    REQUIRE(proc.initialize(cfg));
    CHECK(proc.operational());
    CHECK(proc.startup_error() == StartupError::None);
}

TEST_CASE("an operational processor processes orders after a successful initialize",
          "[startup]") {
    Processor proc;
    REQUIRE(proc.initialize());

    REQUIRE(proc.submit(make_order(1, Side::Buy, 100, 5, 1)));
    REQUIRE(proc.process_next());
    CHECK(proc.processed_count() == 1);
    REQUIRE(proc.engine().book().best_bid() != nullptr);
    CHECK(proc.engine().book().best_bid()->price_ticks == 100);
}

TEST_CASE("initialize resets the book to a clean empty state",
          "[startup]") {
    Processor proc;
    // Rest an order, then re-initialize: the book is cleared back to empty.
    REQUIRE(proc.submit(make_order(1, Side::Buy, 100, 5, 1)));
    REQUIRE(proc.process_next());
    REQUIRE_FALSE(proc.engine().book().empty());

    REQUIRE(proc.initialize());
    CHECK(proc.operational());
    CHECK(proc.engine().book().empty());
}
