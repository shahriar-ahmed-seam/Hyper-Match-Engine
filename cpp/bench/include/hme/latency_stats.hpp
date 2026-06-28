// Per-order Processing_Latency recorder and percentile statistics.
//
// The benchmark harness submits a volume of orders to the Matching_Engine and
// records the Processing_Latency of every *successful* order -- the elapsed
// time, in microseconds, from when the engine dequeues an Order to when it
// emits the corresponding response event. When the run completes the harness
// reports the median and the 99th-percentile of those samples.
//
// This component is the latency *recording + percentile statistics* only:
//
//   * record(sample) appends one Processing_Latency sample (microseconds) to a
//     buffer that is pre-sized once up front, mirroring the "reserve all
//     working memory before the run" spirit of the engine. Recording itself
//     does no sorting and no per-call growth once the reservation is in place.
//   * median(), p99(), and percentile(p) compute order statistics over the
//     recorded samples. count()/min()/max()/empty() expose the rest of the
//     summary surface.
//
// Percentile definition (nearest-rank method)
// --------------------------------------------
// For a requested percentile p in [0, 100] over N (> 0) recorded samples sorted
// ascending as s[0] <= s[1] <= ... <= s[N-1], the nearest-rank value is:
//
//     rank  = ceil( (p / 100) * N )        // 1-based ordinal rank
//     rank  = clamp(rank, 1, N)            // p == 0 maps to rank 1
//     value = s[rank - 1]
//
// The nearest-rank method always returns an *actual member of the sample set*
// (no interpolation), which keeps the values exact integers and guarantees
// monotonicity: because rank is non-decreasing in p, we have min == p0 <=
// median == p50 <= p99 <= p100 == max for any non-empty set. median() is
// defined as the 50th percentile under this same rule.

#ifndef HME_LATENCY_STATS_HPP
#define HME_LATENCY_STATS_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hme::bench {

// Records per-order Processing_Latency samples (microseconds) and computes
// summary order statistics over the successful orders. Not thread-safe: the
// benchmark records on its single driving thread.
class LatencyStats {
public:
    // A latency sample is the Processing_Latency of one successful order,
    // expressed in whole microseconds.
    using sample_type = std::uint64_t;

    LatencyStats() = default;

    // Pre-size the sample buffer for an expected number of successful orders so
    // that record() does not reallocate during the measurement run. Reserving
    // capacity up front does not add any samples; count() stays 0 until the
    // first record().
    explicit LatencyStats(std::size_t expected_samples) {
        samples_.reserve(expected_samples);
    }

    // Append one Processing_Latency sample (microseconds). Only successful
    // orders are recorded; failed/rejected orders are excluded by the caller.
    void record(sample_type latency_us) {
        samples_.push_back(latency_us);
        sorted_valid_ = false;
    }

    // Number of recorded (successful-order) samples.
    std::size_t count() const noexcept { return samples_.size(); }

    // True when no samples have been recorded.
    bool empty() const noexcept { return samples_.empty(); }

    // Smallest recorded sample. Precondition: !empty().
    sample_type min() const {
        ensure_sorted();
        return sorted_.front();
    }

    // Largest recorded sample. Precondition: !empty().
    sample_type max() const {
        ensure_sorted();
        return sorted_.back();
    }

    // Median Processing_Latency = 50th percentile under the nearest-rank rule.
    // Precondition: !empty().
    sample_type median() const { return percentile(50.0); }

    // 99th-percentile Processing_Latency under the nearest-rank rule.
    // Precondition: !empty().
    sample_type p99() const { return percentile(99.0); }

    // Nearest-rank percentile for p in [0, 100] (see file header for the exact
    // formula). p values outside [0, 100] are clamped into range.
    // Precondition: !empty().
    sample_type percentile(double p) const {
        ensure_sorted();
        const std::size_t n = sorted_.size();
        if (p < 0.0) {
            p = 0.0;
        } else if (p > 100.0) {
            p = 100.0;
        }
        // 1-based ordinal rank via the nearest-rank method, then clamp to
        // [1, n] so p == 0 maps to the first element and p == 100 to the last.
        std::size_t rank = static_cast<std::size_t>(ceil_div_pct(p, n));
        if (rank < 1) {
            rank = 1;
        } else if (rank > n) {
            rank = n;
        }
        return sorted_[rank - 1];
    }

private:
    // ceil((p / 100) * n) computed in double then rounded up. Kept in one place
    // so the percentile rounding is defined exactly once.
    static double ceil_div_pct(double p, std::size_t n) {
        const double exact = (p / 100.0) * static_cast<double>(n);
        // Round up to the next ordinal rank; guard tiny FP error so an exact
        // integer rank (e.g. p=50, n=4 -> 2.0) is not bumped to 3.
        const double floored = static_cast<double>(static_cast<std::uint64_t>(exact));
        const double eps = 1e-9;
        if (exact - floored > eps) {
            return floored + 1.0;
        }
        return floored;
    }

    // Rebuild the ascending-sorted view of the samples if a record() has
    // happened since the last sort. Statistics are queried after the run, so
    // sorting lazily (once) keeps record() cheap on the hot recording path.
    void ensure_sorted() const {
        if (sorted_valid_) {
            return;
        }
        sorted_ = samples_;
        std::sort(sorted_.begin(), sorted_.end());
        sorted_valid_ = true;
    }

    std::vector<sample_type> samples_;          // recorded, in arrival order.
    mutable std::vector<sample_type> sorted_;   // cached ascending view.
    mutable bool sorted_valid_ = false;         // is sorted_ up to date?
};

}  // namespace hme::bench

#endif  // HME_LATENCY_STATS_HPP
