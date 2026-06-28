// Unit tests for the Matching_Engine startup memory-reservation FAILURE path.
//
// If the configured Ring_Buffer or Order_Book working memory cannot be reserved
// during startup, the Matching_Engine aborts startup, reports an error
// indicating insufficient working memory, and remains outside the operational
// state so that no Order is processed.
//
// hme::engine::EngineProcessor models the reservation step as
// initialize(EngineConfig). A reservation can fail two ways:
//   * the test-only injection hook EngineConfig::force_reservation_failure, and
//   * a requested capacity that exceeds the reserved fixed capacity.
// Both must drive the same abort-on-failure contract:
//   - initialize() returns false,
//   - startup_error() == StartupError::InsufficientMemory,
//   - operational() == false,
//   - process_next() refuses to run, so a submitted Order is never processed
//     and the Order_Book stays empty.
//
// The success path is covered by the sibling test in engine_startup_test.cpp;
// this file only exercises the failure path.

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

TEST_CASE("forced reservation failure aborts startup and reports insufficient memory",
          "[startup][failure]") {
    Processor proc;
    Processor::EngineConfig cfg;
    cfg.force_reservation_failure = true;

    // Abort-on-failure: initialize() returns false.
    REQUIRE_FALSE(proc.initialize(cfg));
    // Error reported as insufficient working memory.
    CHECK(proc.startup_error() == StartupError::InsufficientMemory);
    // Engine remains outside the operational state.
    CHECK_FALSE(proc.operational());
}

TEST_CASE("requested capacity exceeding the reserved fixed capacity aborts startup",
          "[startup][failure]") {
    // Each requested capacity overflow independently triggers the abort path.
    SECTION("max_orders exceeds reserved capacity") {
        Processor proc;
        Processor::EngineConfig cfg;
        cfg.max_orders = 65;  // > 64 reserved
        REQUIRE_FALSE(proc.initialize(cfg));
        CHECK(proc.startup_error() == StartupError::InsufficientMemory);
        CHECK_FALSE(proc.operational());
    }
    SECTION("max_price_levels exceeds reserved capacity") {
        Processor proc;
        Processor::EngineConfig cfg;
        cfg.max_price_levels = 65;  // > 64 reserved
        REQUIRE_FALSE(proc.initialize(cfg));
        CHECK(proc.startup_error() == StartupError::InsufficientMemory);
        CHECK_FALSE(proc.operational());
    }
    SECTION("ingress_capacity exceeds reserved capacity") {
        Processor proc;
        Processor::EngineConfig cfg;
        cfg.ingress_capacity = 65;  // > 64 reserved
        REQUIRE_FALSE(proc.initialize(cfg));
        CHECK(proc.startup_error() == StartupError::InsufficientMemory);
        CHECK_FALSE(proc.operational());
    }
    SECTION("egress_capacity exceeds reserved capacity") {
        Processor proc;
        Processor::EngineConfig cfg;
        cfg.egress_capacity = 257;  // > 256 reserved
        REQUIRE_FALSE(proc.initialize(cfg));
        CHECK(proc.startup_error() == StartupError::InsufficientMemory);
        CHECK_FALSE(proc.operational());
    }
}

TEST_CASE("a processor whose startup reservation failed processes no Order",
          "[startup][failure]") {
    Processor proc;
    Processor::EngineConfig cfg;
    cfg.force_reservation_failure = true;
    REQUIRE_FALSE(proc.initialize(cfg));
    REQUIRE_FALSE(proc.operational());

    // An Order may still be submitted to the ingress ring, but a non-operational
    // engine must refuse to process it.
    proc.submit(make_order(1, Side::Buy, 100, 5, 1));

    // process_next() returns false (nothing processed) ...
    CHECK_FALSE(proc.process_next());
    CHECK(proc.processed_count() == 0);
    // ... and the Order_Book is left empty: no Order was processed.
    CHECK(proc.engine().book().empty());
    CHECK(proc.engine().book().best_bid() == nullptr);
}
