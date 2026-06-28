// Benchmark failed-order accounting. For a benchmark run driven by an arbitrary
// interleaving of successful orders (each carrying an arbitrary
// Processing_Latency sample) and failed orders:
//
//   * total_orders == successful_orders + failed_orders -- every submitted
//     order is counted exactly once in one of the two buckets.
//   * the latency statistics count() equals ONLY the number of successful
//     orders -- failed/rejected orders are EXCLUDED from latency stats.
//   * failed_orders equals the number of record_failure() calls made.
//
// It generates an arbitrary sequence of per-order outcomes -- a Success carries
// a generated latency sample, a Failure carries none -- drives them through a
// Benchmark, then asserts the three accounting facts above on both the live
// accumulator and the finalized BenchmarkResult.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p25` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstdint>
#include <vector>

#include "hme/benchmark.hpp"
#include "hme/latency_stats.hpp"

using hme::bench::Benchmark;
using hme::bench::BenchmarkConfig;
using hme::bench::BenchmarkResult;
using hme::bench::LatencyStats;

namespace {

// One generated per-order outcome. `is_success` selects Success vs Failure; for
// a Success, `latency_us` is the arbitrary Processing_Latency sample fed into
// the latency stats. For a Failure the latency is ignored (the order is
// excluded from latency stats).
struct OutcomeSpec_p25 {
    bool is_success = true;
    LatencyStats::sample_type latency_us = 0;
};

// Generator for a single outcome: an arbitrary success/failure flag paired with
// a latency sample drawn from a wide-but-bounded microsecond range so successes
// exercise varied Processing_Latency values without overflow concerns.
rc::Gen<OutcomeSpec_p25> gen_outcome_p25() {
    return rc::gen::map(
        rc::gen::pair(
            rc::gen::arbitrary<bool>(),
            rc::gen::inRange<LatencyStats::sample_type>(0, 1'000'001)),
        [](const std::pair<bool, LatencyStats::sample_type>& p) {
            return OutcomeSpec_p25{p.first, p.second};
        });
}

}  // namespace

TEST_CASE(
    "Property 25: total == successful + failed; latency count == successful "
    "only; failed == record_failure calls",
    "[bench][property][failed-order-accounting]") {
    const bool ok = rc::check(
        "benchmark failed-order accounting excludes failures from latency stats",
        [] {
            // An arbitrary interleaving of successes (with latencies) and
            // failures. Empty sequences are allowed and exercise the zero-order
            // edge case.
            const std::vector<OutcomeSpec_p25> outcomes =
                *rc::gen::container<std::vector<OutcomeSpec_p25>>(
                    gen_outcome_p25());

            // order_volume is kept within the valid range so construction
            // succeeds; the run records its own outcomes regardless of the
            // configured volume (which only pre-sizes the latency buffer).
            BenchmarkConfig config;
            config.order_volume = 1'000;
            Benchmark benchmark(config);

            // Independently count what we submit so the assertions compare the
            // benchmark's accounting against an external ground truth.
            std::uint64_t expected_successful = 0;
            std::uint64_t expected_failed = 0;
            for (const auto& o : outcomes) {
                if (o.is_success) {
                    benchmark.record_success(o.latency_us);
                    ++expected_successful;
                } else {
                    benchmark.record_failure();
                    ++expected_failed;
                }
            }

            const std::uint64_t total =
                expected_successful + expected_failed;

            // Live accumulator accounting.
            RC_ASSERT(benchmark.successful_orders() == expected_successful);
            RC_ASSERT(benchmark.failed_orders() == expected_failed);
            RC_ASSERT(benchmark.processed_orders() == total);

            // Failed orders are EXCLUDED from latency stats, so the latency
            // sample count equals the successful count only.
            RC_ASSERT(benchmark.latency().count() ==
                      static_cast<std::size_t>(expected_successful));

            // The finalized result reports the same accounting. The elapsed
            // time only affects throughput, not the order counts, so any
            // positive value works here.
            const BenchmarkResult result = benchmark.finalize(1.0);
            RC_ASSERT(result.successful_orders == expected_successful);
            RC_ASSERT(result.failed_orders == expected_failed);
            RC_ASSERT(result.total_orders ==
                      result.successful_orders + result.failed_orders);
            RC_ASSERT(result.total_orders == total);
        });
    CHECK(ok);
}
