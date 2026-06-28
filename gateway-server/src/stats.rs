//! Latency and throughput accounting for forwarded requests.
//!
//! Round-trip latency (submit → terminating Ack/Reject) is measured in
//! microseconds and kept in a bounded rolling window so p50/p99/max can be
//! computed cheaply. Throughput is the number of orders submitted within the
//! most recent one-second window.

use std::collections::VecDeque;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use serde::Serialize;

/// Maximum number of latency samples retained in the rolling window.
const LATENCY_WINDOW: usize = 100_000;
/// Window over which rolling throughput is computed.
const THROUGHPUT_WINDOW: Duration = Duration::from_secs(1);

#[derive(Debug, Default)]
struct Counters {
    orders_submitted: u64,
    orders_accepted: u64,
    orders_rejected: u64,
    cancels: u64,
    trades: u64,
}

#[derive(Debug)]
struct Inner {
    counters: Counters,
    latencies: VecDeque<u64>,
    submissions: VecDeque<Instant>,
}

/// Thread-safe metrics accumulator.
#[derive(Debug)]
pub struct Metrics {
    inner: Mutex<Inner>,
}

/// Computed latency percentiles over the current window (microseconds).
#[derive(Debug, Clone, Copy, Serialize)]
pub struct LatencySummary {
    pub p50: u64,
    pub p99: u64,
    pub max: u64,
}

/// A point-in-time view of all metrics, ready to serialize for the API/WS.
#[derive(Debug, Clone)]
pub struct MetricsSnapshot {
    pub orders_submitted: u64,
    pub orders_accepted: u64,
    pub orders_rejected: u64,
    pub cancels: u64,
    pub trades: u64,
    pub latency_us: LatencySummary,
    pub throughput_ops: u64,
}

impl Default for Metrics {
    fn default() -> Self {
        Self::new()
    }
}

impl Metrics {
    pub fn new() -> Self {
        Metrics {
            inner: Mutex::new(Inner {
                counters: Counters::default(),
                latencies: VecDeque::with_capacity(1024),
                submissions: VecDeque::new(),
            }),
        }
    }

    /// Record that an order was submitted to the engine (for throughput).
    pub fn record_submission(&self) {
        let mut inner = self.inner.lock().unwrap();
        inner.counters.orders_submitted += 1;
        let now = Instant::now();
        inner.submissions.push_back(now);
        prune(&mut inner.submissions, now);
    }

    /// Record a completed round-trip latency sample (microseconds).
    pub fn record_latency(&self, micros: u64) {
        let mut inner = self.inner.lock().unwrap();
        if inner.latencies.len() == LATENCY_WINDOW {
            inner.latencies.pop_front();
        }
        inner.latencies.push_back(micros);
    }

    pub fn record_accepted(&self) {
        self.inner.lock().unwrap().counters.orders_accepted += 1;
    }

    pub fn record_rejected(&self) {
        self.inner.lock().unwrap().counters.orders_rejected += 1;
    }

    pub fn record_cancel(&self) {
        self.inner.lock().unwrap().counters.cancels += 1;
    }

    pub fn record_trade(&self) {
        self.inner.lock().unwrap().counters.trades += 1;
    }

    /// Produce a consistent snapshot of all metrics.
    pub fn snapshot(&self) -> MetricsSnapshot {
        let mut inner = self.inner.lock().unwrap();
        let now = Instant::now();
        prune(&mut inner.submissions, now);
        let throughput_ops = inner.submissions.len() as u64;
        let latency_us = percentiles(&inner.latencies);
        MetricsSnapshot {
            orders_submitted: inner.counters.orders_submitted,
            orders_accepted: inner.counters.orders_accepted,
            orders_rejected: inner.counters.orders_rejected,
            cancels: inner.counters.cancels,
            trades: inner.counters.trades,
            latency_us,
            throughput_ops,
        }
    }
}

/// Drop submission timestamps older than the throughput window.
fn prune(submissions: &mut VecDeque<Instant>, now: Instant) {
    while let Some(&front) = submissions.front() {
        if now.duration_since(front) > THROUGHPUT_WINDOW {
            submissions.pop_front();
        } else {
            break;
        }
    }
}

/// Compute p50/p99/max over the latency window.
fn percentiles(latencies: &VecDeque<u64>) -> LatencySummary {
    if latencies.is_empty() {
        return LatencySummary {
            p50: 0,
            p99: 0,
            max: 0,
        };
    }
    let mut sorted: Vec<u64> = latencies.iter().copied().collect();
    sorted.sort_unstable();
    let last = sorted.len() - 1;
    let p50 = sorted[(sorted.len() * 50 / 100).min(last)];
    let p99 = sorted[(sorted.len() * 99 / 100).min(last)];
    let max = sorted[last];
    LatencySummary { p50, p99, max }
}
