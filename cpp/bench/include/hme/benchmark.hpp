// Benchmark throughput measurement, failed-order accounting, and pass/fail.
//
// Builds on the latency recorder (latency_stats.hpp) to form the benchmark
// harness' result surface:
//
//   * BenchmarkConfig carries the configurable order volume -- settable within
//     1,000 .. 1,000,000,000 orders -- and the throughput target to compare
//     against. The target defaults to 100,000 orders/sec but is configurable so
//     the harness can be exercised with other targets in tests. The hardware
//     target itself is validated on representative hardware, not in CI; here we
//     only *compare* the measured throughput against the configured target.
//
//   * Benchmark accumulates per-order outcomes as the run proceeds:
//       - record_success(latency_us) counts a fully-processed order and feeds
//         its Processing_Latency into the LatencyStats recorder.
//       - record_failure() counts an order that was rejected or failed to be
//         processed; such orders are EXCLUDED from the latency statistics.
//
//   * finalize(elapsed_seconds) produces a BenchmarkResult reporting the total
//     orders processed and the elapsed wall-clock time, the measured
//     Sustained_Throughput in orders/sec, and whether the target was met. When
//     the throughput is below target the result is a non-success that still
//     reports the achieved throughput.
//
// Timing is *injected* rather than measured internally: finalize() takes the
// elapsed wall-clock seconds as a parameter. This keeps the result logic a pure
// function of (counts, elapsed, target) so it is unit-testable on small volumes
// without running a real multi-second benchmark. A live driver measures the
// window with a steady clock and hands the elapsed time to finalize().

#ifndef HME_BENCHMARK_HPP
#define HME_BENCHMARK_HPP

#include <cstdint>
#include <stdexcept>

#include "hme/latency_stats.hpp"

namespace hme::bench {

// Configurable order-volume bounds: the benchmark submits a volume settable
// within 1,000 .. 1,000,000,000 orders, inclusive.
inline constexpr std::uint64_t kMinOrderVolume = 1'000ULL;
inline constexpr std::uint64_t kMaxOrderVolume = 1'000'000'000ULL;

// Default Sustained_Throughput target in orders/sec. The pass/fail comparison
// uses the configured target; this is just the default when none is supplied.
inline constexpr double kDefaultThroughputTarget = 100'000.0;

// Disposition of a single submitted order during a benchmark run. A Success is
// an order the Matching_Engine fully processed (and for which a Processing_
// Latency sample exists); a Failure is an order that was rejected or otherwise
// failed to be processed.
enum class OrderOutcome { Success, Failure };

// Configuration for a benchmark run: how many orders to submit and the
// throughput target to judge the run against.
struct BenchmarkConfig {
    // Number of orders the benchmark submits. Must be within
    // [kMinOrderVolume, kMaxOrderVolume].
    std::uint64_t order_volume = kMinOrderVolume;

    // Sustained_Throughput target in orders/sec the run must meet or exceed to
    // be considered a success. Must be > 0.
    double target_throughput = kDefaultThroughputTarget;

    // True when order_volume is within the configurable range.
    static constexpr bool volume_in_range(std::uint64_t volume) noexcept {
        return volume >= kMinOrderVolume && volume <= kMaxOrderVolume;
    }

    // True when this configuration is usable: volume in range and a positive
    // throughput target.
    constexpr bool valid() const noexcept {
        return volume_in_range(order_volume) && target_throughput > 0.0;
    }
};

// Outcome of a completed benchmark run. Reported once finalize() is called.
struct BenchmarkResult {
    // The configured order volume the run was asked to submit.
    std::uint64_t configured_volume = 0;

    // Total orders processed = successful + failed.
    std::uint64_t total_orders = 0;

    // Orders the engine fully processed; the basis for latency statistics.
    std::uint64_t successful_orders = 0;

    // Orders rejected or that failed to be processed, excluded from latency
    // statistics.
    std::uint64_t failed_orders = 0;

    // Elapsed wall-clock time of the measurement window, in seconds.
    double elapsed_seconds = 0.0;

    // Measured Sustained_Throughput in orders/sec.
    double sustained_throughput = 0.0;

    // The throughput target the run was judged against.
    double target_throughput = kDefaultThroughputTarget;

    // True when the measured throughput met or exceeded the target. When false
    // the run is a non-success and sustained_throughput reports the achieved
    // (below-target) value.
    bool met_target = false;

    // A successful run is one that met the throughput target. A below-target
    // run is a non-success that still carries the achieved throughput.
    constexpr bool success() const noexcept { return met_target; }
};

// Accumulates per-order outcomes during a benchmark run and produces a
// BenchmarkResult. Not thread-safe: the benchmark drives this from its single
// submitting thread, mirroring the engine's single-threaded model.
class Benchmark {
public:
    // Construct with a validated configuration. Throws std::invalid_argument if
    // the order volume is outside [kMinOrderVolume, kMaxOrderVolume] or the
    // target throughput is not positive. The latency recorder is pre-sized to
    // the configured volume so record_success() does not grow its buffer
    // mid-run.
    explicit Benchmark(BenchmarkConfig config)
        : config_(config),
          latency_(config.valid()
                       ? static_cast<std::size_t>(config.order_volume)
                       : 0) {
        if (!config_.valid()) {
            throw std::invalid_argument(
                "BenchmarkConfig: order_volume must be within "
                "[1000, 1000000000] and target_throughput must be positive");
        }
    }

    // Record one fully-processed order with its Processing_Latency in
    // microseconds. The sample is added to the latency statistics.
    void record_success(LatencyStats::sample_type latency_us) {
        ++successful_orders_;
        latency_.record(latency_us);
    }

    // Record one order that was rejected or failed to be processed. Counted as a
    // failure and EXCLUDED from the latency statistics.
    void record_failure() noexcept { ++failed_orders_; }

    // Convenience: record an outcome with an associated latency. The latency is
    // only used for a Success; for a Failure it is ignored (the order is
    // excluded from latency stats).
    void record(OrderOutcome outcome, LatencyStats::sample_type latency_us = 0) {
        if (outcome == OrderOutcome::Success) {
            record_success(latency_us);
        } else {
            record_failure();
        }
    }

    // Orders fully processed so far.
    std::uint64_t successful_orders() const noexcept { return successful_orders_; }

    // Orders rejected / failed so far.
    std::uint64_t failed_orders() const noexcept { return failed_orders_; }

    // Total orders processed so far = successful + failed.
    std::uint64_t processed_orders() const noexcept {
        return successful_orders_ + failed_orders_;
    }

    // The configuration this run was constructed with.
    const BenchmarkConfig& config() const noexcept { return config_; }

    // Latency statistics over the successful orders only.
    const LatencyStats& latency() const noexcept { return latency_; }
    LatencyStats& latency() noexcept { return latency_; }

    // Produce the run result given the measured elapsed wall-clock time, in
    // seconds, of the measurement window. Sustained_Throughput is the total
    // orders processed divided by the elapsed time; a non-positive elapsed time
    // yields a throughput of 0 (no orders/sec can be attributed). The result
    // reports totals and elapsed time and whether the configured target was met;
    // when below target the result is a non-success carrying the achieved
    // throughput.
    BenchmarkResult finalize(double elapsed_seconds) const {
        BenchmarkResult result;
        result.configured_volume = config_.order_volume;
        result.successful_orders = successful_orders_;
        result.failed_orders = failed_orders_;
        result.total_orders = processed_orders();
        result.elapsed_seconds = elapsed_seconds;
        result.target_throughput = config_.target_throughput;
        result.sustained_throughput =
            elapsed_seconds > 0.0
                ? static_cast<double>(result.total_orders) / elapsed_seconds
                : 0.0;
        result.met_target =
            result.sustained_throughput >= config_.target_throughput;
        return result;
    }

private:
    BenchmarkConfig config_;
    LatencyStats latency_;
    std::uint64_t successful_orders_ = 0;
    std::uint64_t failed_orders_ = 0;
};

}  // namespace hme::bench

#endif  // HME_BENCHMARK_HPP
