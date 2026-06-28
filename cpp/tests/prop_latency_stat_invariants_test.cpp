// Latency statistic invariants. For any non-empty set of recorded per-order
// Processing_Latency samples, the reported median and 99th-percentile satisfy
// min <= median <= p99 <= max, and both reported values are members of the
// sample set (the nearest-rank method returns actual samples, no interpolation).
//
// Over an arbitrary non-empty multiset of latency samples it asserts the full
// invariant surface of LatencyStats:
//
//   1. Ordering:        min <= median <= p99 <= max.
//   2. Membership:      median and p99 are actual members of the sample set
//                       (nearest-rank returns recorded values, never an
//                       interpolated/synthetic one).
//   3. Monotonicity:    percentile(p) is non-decreasing as p increases over
//                       [0, 100], so the reported order statistics never invert.
//   4. Count fidelity:  count() equals the number of samples recorded.
//   5. Order invariance: recording the same multiset in a different (shuffled)
//                        order yields identical median and p99 -- the statistics
//                        depend only on the set of values, not arrival order.
//
// Generators are defined locally in an anonymous namespace with names suffixed
// `_p24` so they do not clash (ODR) with generators in other property-test TUs.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

#include "hme/latency_stats.hpp"

using hme::bench::LatencyStats;

namespace {

using Sample_p24 = LatencyStats::sample_type;

// One Processing_Latency sample in microseconds. The domain is small enough
// that duplicate values occur frequently (so membership and order-invariance
// are exercised on multisets, not just distinct sets) yet wide enough to make
// the percentile ranks non-trivial.
rc::Gen<Sample_p24> gen_sample_p24() {
    return rc::gen::inRange<Sample_p24>(0, 5000);
}

// A non-empty multiset of latency samples in arrival order. The property is
// stated over non-empty sample sets, so the generator guarantees at least one
// element.
rc::Gen<std::vector<Sample_p24>> gen_samples_p24() {
    return rc::gen::nonEmpty(
        rc::gen::container<std::vector<Sample_p24>>(gen_sample_p24()));
}

// Record every sample into a fresh LatencyStats, in the given order.
LatencyStats record_all_p24(const std::vector<Sample_p24>& samples) {
    LatencyStats stats(samples.size());
    for (Sample_p24 s : samples) {
        stats.record(s);
    }
    return stats;
}

}  // namespace

TEST_CASE(
    "Property 24: latency statistics are ordered, drawn from the sample set, "
    "monotonic in p, count-faithful, and independent of insertion order",
    "[bench][latency][property]") {
    const bool ok = rc::check(
        "min<=median<=p99<=max; median/p99 are members; percentile monotonic; "
        "count faithful; order-invariant",
        [] {
            const std::vector<Sample_p24> samples = *gen_samples_p24();

            const LatencyStats stats = record_all_p24(samples);

            // (4) Count fidelity: every recorded sample is counted exactly once.
            RC_ASSERT(stats.count() == samples.size());

            const Sample_p24 lo = stats.min();
            const Sample_p24 med = stats.median();
            const Sample_p24 p99 = stats.p99();
            const Sample_p24 hi = stats.max();

            // (1) Ordering: min <= median <= p99 <= max.
            RC_ASSERT(lo <= med);
            RC_ASSERT(med <= p99);
            RC_ASSERT(p99 <= hi);

            // (2) Membership: median and p99 (and the reported min/max) are
            // actual members of the recorded sample set.
            const std::unordered_set<Sample_p24> value_set(samples.begin(),
                                                           samples.end());
            RC_ASSERT(value_set.count(lo) == 1);
            RC_ASSERT(value_set.count(med) == 1);
            RC_ASSERT(value_set.count(p99) == 1);
            RC_ASSERT(value_set.count(hi) == 1);

            // (3) Monotonicity: percentile(p) is non-decreasing in p across the
            // whole [0, 100] range. Stepping by 1 percent covers every distinct
            // nearest-rank threshold the percentile can produce.
            Sample_p24 prev = stats.percentile(0.0);
            RC_ASSERT(prev == lo);  // p == 0 maps to the minimum.
            for (int pct = 1; pct <= 100; ++pct) {
                const Sample_p24 cur = stats.percentile(static_cast<double>(pct));
                RC_ASSERT(cur >= prev);
                prev = cur;
            }
            RC_ASSERT(stats.percentile(100.0) == hi);  // p == 100 maps to max.

            // (5) Order invariance: recording the same multiset in a different
            // arrival order yields identical statistics. A RapidCheck-supplied
            // seed drives a deterministic shuffle so the permutation is
            // arbitrary yet reproducible on shrink/replay.
            const std::uint64_t shuffle_seed = *rc::gen::arbitrary<std::uint64_t>();
            std::vector<Sample_p24> shuffled = samples;
            std::mt19937_64 rng(shuffle_seed);
            std::shuffle(shuffled.begin(), shuffled.end(), rng);

            const LatencyStats shuffled_stats = record_all_p24(shuffled);
            RC_ASSERT(shuffled_stats.count() == stats.count());
            RC_ASSERT(shuffled_stats.min() == lo);
            RC_ASSERT(shuffled_stats.median() == med);
            RC_ASSERT(shuffled_stats.p99() == p99);
            RC_ASSERT(shuffled_stats.max() == hi);
        });
    CHECK(ok);
}
