// Unit tests for the benchmark latency recorder + percentile statistics.
//
// These verify record()/count() and the nearest-rank median()/p99()/percentile()
// definitions on concrete examples and edge cases. The numbered correctness
// property for latency statistics is covered separately by the RapidCheck
// property test.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "hme/latency_stats.hpp"

using hme::bench::LatencyStats;

TEST_CASE("LatencyStats starts empty and counts recorded samples", "[bench][latency]") {
    LatencyStats stats;
    CHECK(stats.count() == 0);
    CHECK(stats.empty());

    stats.record(10);
    stats.record(20);
    stats.record(30);
    CHECK(stats.count() == 3);
    CHECK_FALSE(stats.empty());
}

TEST_CASE("LatencyStats records out-of-order samples and sorts for statistics",
          "[bench][latency]") {
    LatencyStats stats;
    // Intentionally not sorted on input.
    for (std::uint64_t s : {50u, 10u, 40u, 20u, 30u}) {
        stats.record(s);
    }
    CHECK(stats.count() == 5);
    CHECK(stats.min() == 10);
    CHECK(stats.max() == 50);
}

TEST_CASE("LatencyStats single sample reports that sample for every statistic",
          "[bench][latency]") {
    LatencyStats stats;
    stats.record(42);
    CHECK(stats.min() == 42);
    CHECK(stats.max() == 42);
    CHECK(stats.median() == 42);
    CHECK(stats.p99() == 42);
}

TEST_CASE("LatencyStats median is the nearest-rank 50th percentile", "[bench][latency]") {
    SECTION("odd count -> middle element") {
        LatencyStats stats;
        for (std::uint64_t s : {1u, 2u, 3u, 4u, 5u}) {
            stats.record(s);
        }
        // rank = ceil(0.50 * 5) = ceil(2.5) = 3 -> sorted[2] == 3.
        CHECK(stats.median() == 3);
    }
    SECTION("even count -> upper-middle element (nearest-rank)") {
        LatencyStats stats;
        for (std::uint64_t s : {10u, 20u, 30u, 40u}) {
            stats.record(s);
        }
        // rank = ceil(0.50 * 4) = 2 -> sorted[1] == 20.
        CHECK(stats.median() == 20);
    }
}

TEST_CASE("LatencyStats p99 is the nearest-rank 99th percentile", "[bench][latency]") {
    // 100 samples with values 1..100; p99 -> rank = ceil(0.99 * 100) = 99
    // -> sorted[98] == 99.
    LatencyStats stats(100);
    for (std::uint64_t v = 1; v <= 100; ++v) {
        stats.record(v);
    }
    CHECK(stats.count() == 100);
    CHECK(stats.median() == 50);  // rank = ceil(0.5*100) = 50 -> sorted[49] == 50.
    CHECK(stats.p99() == 99);
    CHECK(stats.max() == 100);
}

TEST_CASE("LatencyStats percentile clamps p=0 to min and p=100 to max",
          "[bench][latency]") {
    LatencyStats stats;
    for (std::uint64_t s : {7u, 3u, 9u, 1u, 5u}) {
        stats.record(s);
    }
    CHECK(stats.percentile(0.0) == 1);    // min
    CHECK(stats.percentile(100.0) == 9);  // max
    // Out-of-range inputs clamp into [0, 100].
    CHECK(stats.percentile(-5.0) == 1);
    CHECK(stats.percentile(150.0) == 9);
}

TEST_CASE("LatencyStats reported statistics are ordered min<=median<=p99<=max",
          "[bench][latency]") {
    LatencyStats stats;
    for (std::uint64_t s : {120u, 5u, 60u, 5u, 999u, 30u, 7u, 60u, 11u}) {
        stats.record(s);
    }
    CHECK(stats.min() <= stats.median());
    CHECK(stats.median() <= stats.p99());
    CHECK(stats.p99() <= stats.max());
}

TEST_CASE("LatencyStats recomputes statistics after additional records",
          "[bench][latency]") {
    LatencyStats stats;
    stats.record(100);
    CHECK(stats.max() == 100);  // forces an internal sort.
    stats.record(5);
    stats.record(250);
    // New samples must be reflected after the earlier query.
    CHECK(stats.min() == 5);
    CHECK(stats.max() == 250);
    CHECK(stats.count() == 3);
}
