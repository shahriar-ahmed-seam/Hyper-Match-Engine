//! Live order-book projection derived purely from real engine events.
//!
//! The projection is not a simulation: it is updated only from the terminating
//! `Ack`/`Reject` events and the `Trade` events the Matching_Engine emits. The
//! Gateway knows each forwarded order's submitted side/price/quantity (it sent
//! the order), so an accepted `NewOrder` can be reduced by its fills to compute
//! the quantity that came to rest.

use std::collections::{BTreeMap, HashMap};

use codec::Side;
use serde::Serialize;

/// A single resting order tracked by the projection.
#[derive(Debug, Clone, Copy)]
struct RestingOrder {
    side: Side,
    price_ticks: u64,
    remaining: u32,
}

/// Aggregated quantity and order count at a single price level.
#[derive(Debug, Default, Clone, Copy)]
struct Level {
    quantity: u64,
    orders: u32,
}

/// One aggregated price level in a book snapshot, serialized for the API.
#[derive(Debug, Clone, Serialize)]
pub struct BookLevel {
    pub price: f64,
    pub quantity: u64,
    pub orders: u32,
}

/// A depth-limited snapshot of both sides of the book plus the book sequence.
#[derive(Debug, Clone, Serialize)]
pub struct BookSnapshot {
    pub bids: Vec<BookLevel>,
    pub asks: Vec<BookLevel>,
    pub sequence: u64,
}

/// The derived order-book projection.
#[derive(Debug, Default)]
pub struct OrderBook {
    orders: HashMap<u64, RestingOrder>,
    bids: BTreeMap<u64, Level>,
    asks: BTreeMap<u64, Level>,
    sequence: u64,
}

impl OrderBook {
    /// Number of resting orders currently tracked.
    pub fn resting_count(&self) -> usize {
        self.orders.len()
    }

    fn levels_mut(&mut self, side: Side) -> &mut BTreeMap<u64, Level> {
        match side {
            Side::Buy => &mut self.bids,
            Side::Sell => &mut self.asks,
        }
    }

    /// Insert a newly resting order (the residual of an accepted `NewOrder`).
    pub fn insert_resting(&mut self, order_id: u64, side: Side, price_ticks: u64, remaining: u32) {
        if remaining == 0 {
            return;
        }
        // Replacing an id that somehow already rests would corrupt the level
        // aggregates, so drop any prior tracking of it first.
        if self.orders.contains_key(&order_id) {
            self.remove(order_id);
        }
        self.orders.insert(
            order_id,
            RestingOrder {
                side,
                price_ticks,
                remaining,
            },
        );
        let level = self.levels_mut(side).entry(price_ticks).or_default();
        level.quantity += remaining as u64;
        level.orders += 1;
        self.sequence += 1;
    }

    /// Reduce a resting order (identified by `resting_id`) by a filled quantity,
    /// removing it from the book once fully consumed.
    pub fn apply_trade(&mut self, resting_id: u64, quantity: u32) {
        let (side, price_ticks, decrement, emptied) = match self.orders.get_mut(&resting_id) {
            Some(order) => {
                let decrement = quantity.min(order.remaining);
                order.remaining -= decrement;
                (
                    order.side,
                    order.price_ticks,
                    decrement,
                    order.remaining == 0,
                )
            }
            None => return,
        };

        if let Some(level) = self.levels_mut(side).get_mut(&price_ticks) {
            level.quantity = level.quantity.saturating_sub(decrement as u64);
            if emptied {
                level.orders = level.orders.saturating_sub(1);
            }
            if level.orders == 0 || level.quantity == 0 {
                self.levels_mut(side).remove(&price_ticks);
            }
        }

        if emptied {
            self.orders.remove(&resting_id);
        }
        self.sequence += 1;
    }

    /// Remove a resting order outright (a cancellation).
    pub fn remove(&mut self, order_id: u64) {
        let Some(order) = self.orders.remove(&order_id) else {
            return;
        };
        if let Some(level) = self.levels_mut(order.side).get_mut(&order.price_ticks) {
            level.quantity = level.quantity.saturating_sub(order.remaining as u64);
            level.orders = level.orders.saturating_sub(1);
            if level.orders == 0 || level.quantity == 0 {
                self.levels_mut(order.side).remove(&order.price_ticks);
            }
        }
        self.sequence += 1;
    }

    /// Build a depth-limited snapshot: bids ordered high→low, asks low→high.
    pub fn snapshot(&self, depth: usize) -> BookSnapshot {
        let bids = self
            .bids
            .iter()
            .rev()
            .take(depth)
            .map(|(&price_ticks, level)| BookLevel {
                price: ticks_to_price(price_ticks),
                quantity: level.quantity,
                orders: level.orders,
            })
            .collect();
        let asks = self
            .asks
            .iter()
            .take(depth)
            .map(|(&price_ticks, level)| BookLevel {
                price: ticks_to_price(price_ticks),
                quantity: level.quantity,
                orders: level.orders,
            })
            .collect();
        BookSnapshot {
            bids,
            asks,
            sequence: self.sequence,
        }
    }
}

/// Convert fixed-point `price_ticks` (price × 100) back into a decimal price.
pub fn ticks_to_price(price_ticks: u64) -> f64 {
    price_ticks as f64 / 100.0
}
