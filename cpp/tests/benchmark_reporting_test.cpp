// Unit tests for benchmark reporting.
//
// These exercise the Benchmark harness' result surface on small, fully
// deterministic volumes: timing is injected into finalize(elapsed_seconds) so
// the reporting logic is a pure function of (counts, elapsed, target) and can
// be asserted exactly without running a real multi-second benchmark.
//
// Coverage:
//   * A small-volume run records both latency (successful orders feed the
//     LatencyStats recorder) and throughput, and the result reports the totals
//     and elapsed wall-clock time.
//   * A run whose computed Sustained_Throughput is below the configured target
//     yields a non-success result that still carries the achieved
//     (below-target) throughput.
//   * A run meeting or exceeding the target is a success.
//   * Constructing with an out-of-range order volume or a non-positive target
//     throughput throws std::invalid_argument.
//   * The zero-elapsed guard yields a throughput of 0.
//
// The numbered correctness properties are covered separately by the RapidCheck
// property tests.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <stdexcept>

#include "hme/benchmark.hpp"

using Catch::Matchers::WithinRel;
using hme::bench::Benchmark;
using hme::bench::BenchmarkConfig;
using hme::bench::BenchmarkResult;
using hme::bench::kMaxOrderVolume;
using hme::bench::kMinOrderVolume;
using hme::bench::OrderOutcome;

TEST_CASE(
    "Benchmark small-volume run records latency and throughput and reports "
    "totals and elapsed time",
    "[bench][reporting]") {
    // Small volume well within [1000, 1e9]; modest target so the run passes.
    BenchmarkConfig config;
    config.order_volume = 1'000;
    config.target_throughput = 1'000.0;  // orders/sec

    Benchmark bench(config);

    // Submit 8 orders: 6 successful (with latency samples) + 2 failed.
    bench.record_success(10);
    bench.record_success(20);
    bench.record_success(30);
    bench.record_success(40);
    bench.record_success(50);
    bench.record_success(60);
    bench.record_failure();
    bench.record_failure();

    // Latency is recorded for successful orders only: 6 samples feed the
    // recorder; failures are excluded.
    CHECK(bench.successful_orders() == 6);
    CHECK(bench.failed_orders() == 2);
    CHECK(bench.processed_orders() == 8);
    CHECK(bench.latency().count() == 6);
    CHECK(bench.latency().min() == 10);
    CHECK(bench.latency().max() == 60);

    // Measurement window of 0.004 s -> 8 / 0.004 = 2000 orders/sec.
    const BenchmarkResult result = bench.finalize(0.004);

    // Reports totals and elapsed time.
    CHECK(result.configured_volume == 1'000);
    CHECK(result.total_orders == 8);
    CHECK(result.successful_orders == 6);
    CHECK(result.failed_orders == 2);
    CHECK_THAT(result.elapsed_seconds, WithinRel(0.004, 1e-9));

    // Measured Sustained_Throughput.
    CHECK_THAT(result.sustained_throughput, WithinRel(2'000.0, 1e-9));

    // 2000 >= 1000 target -> success.
    CHECK(result.met_target);
    CHECK(result.success());
}

TEST_CASE(
    "Benchmark below-target run is a non-success that still reports the "
    "achieved throughput",
    "[bench][reporting]") {
    BenchmarkConfig config;
    config.order_volume = 10'000;
    config.target_throughput = 100'000.0;  // orders/sec target

    Benchmark bench(config);

    // 5 orders processed, all successful.
    for (int i = 0; i < 5; ++i) {
        bench.record_success(static_cast<std::uint64_t>(100 + i));
    }

    // Elapsed 0.001 s -> 5 / 0.001 = 5000 orders/sec, far below the 100k target.
    const BenchmarkResult result = bench.finalize(0.001);

    CHECK(result.total_orders == 5);
    CHECK_THAT(result.sustained_throughput, WithinRel(5'000.0, 1e-9));

    // Below target -> non-success...
    CHECK_FALSE(result.met_target);
    CHECK_FALSE(result.success());
    // ...but the achieved (below-target) throughput is still carried.
    CHECK(result.target_throughput == 100'000.0);
    CHECK(result.sustained_throughput < result.target_throughput);
    CHECK(result.sustained_throughput > 0.0);
}

TEST_CASE("Benchmark run that meets or exceeds the target is a success",
          "[bench][reporting]") {
    BenchmarkConfig config;
    config.order_volume = 1'000;
    config.target_throughput = 1'000.0;

    SECTION("exactly meeting the target is a success") {
        Benchmark bench(config);
        // 10 orders in 0.01 s -> exactly 1000 orders/sec == target.
        for (int i = 0; i < 10; ++i) {
            bench.record_success(5);
        }
        const BenchmarkResult result = bench.finalize(0.01);
        CHECK_THAT(result.sustained_throughput, WithinRel(1'000.0, 1e-9));
        CHECK(result.met_target);
        CHECK(result.success());
    }

    SECTION("exceeding the target is a success") {
        Benchmark bench(config);
        // 10 orders in 0.001 s -> 10000 orders/sec > target.
        for (int i = 0; i < 10; ++i) {
            bench.record_success(5);
        }
        const BenchmarkResult result = bench.finalize(0.001);
        CHECK(result.sustained_throughput > result.target_throughput);
        CHECK(result.met_target);
        CHECK(result.success());
    }
}

TEST_CASE(
    "Benchmark construction rejects out-of-range volume or non-positive target",
    "[bench][reporting]") {
    SECTION("volume below the minimum throws") {
        BenchmarkConfig config;
        config.order_volume = kMinOrderVolume - 1;  // 999
        CHECK_THROWS_AS(Benchmark(config), std::invalid_argument);
    }

    SECTION("volume above the maximum throws") {
        BenchmarkConfig config;
        config.order_volume = kMaxOrderVolume + 1;  // 1,000,000,001
        CHECK_THROWS_AS(Benchmark(config), std::invalid_argument);
    }

    SECTION("zero target throughput throws") {
        BenchmarkConfig config;
        config.order_volume = kMinOrderVolume;
        config.target_throughput = 0.0;
        CHECK_THROWS_AS(Benchmark(config), std::invalid_argument);
    }

    SECTION("negative target throughput throws") {
        BenchmarkConfig config;
        config.order_volume = kMinOrderVolume;
        config.target_throughput = -1.0;
        CHECK_THROWS_AS(Benchmark(config), std::invalid_argument);
    }

    SECTION("volume at the inclusive bounds is accepted") {
        BenchmarkConfig low;
        low.order_volume = kMinOrderVolume;
        CHECK_NOTHROW(Benchmark(low));

        BenchmarkConfig high;
        high.order_volume = kMaxOrderVolume;
        CHECK_NOTHROW(Benchmark(high));
    }
}

TEST_CASE("Benchmark zero or non-positive elapsed time yields a throughput of 0",
          "[bench][reporting]") {
    BenchmarkConfig config;
    config.order_volume = 1'000;
    config.target_throughput = 1'000.0;

    Benchmark bench(config);
    bench.record_success(10);
    bench.record_success(20);
    bench.record_failure();

    SECTION("zero elapsed seconds") {
        const BenchmarkResult result = bench.finalize(0.0);
        CHECK(result.total_orders == 3);
        CHECK(result.sustained_throughput == 0.0);
        // 0 < positive target -> not met.
        CHECK_FALSE(result.met_target);
    }

    SECTION("negative elapsed seconds (defensive guard)") {
        const BenchmarkResult result = bench.finalize(-1.0);
        CHECK(result.sustained_throughput == 0.0);
        CHECK_FALSE(result.met_target);
    }
}
