//! Property-based test for valid order normalization by the Gateway.
//!
//! For any JSON order with a valid side, an in-range price, and an in-range
//! integer quantity, the Gateway accepts it and yields a normalized
//! `ValidatedOrder`. Lives in its own integration-test file (one property per
//! test) to avoid conflicts with other test files that edit
//! `gateway/src/lib.rs`.

use std::collections::HashSet;

use codec::{limits, BinaryMessage, Side};
use gateway::{Gateway, ValidatedOrder};
use proptest::prelude::*;

/// One generated order: a valid side, an in-range price (derived from in-range
/// `price_ticks` so acceptance is guaranteed), an in-range integer quantity,
/// and an optional supplied identifier.
#[derive(Debug, Clone)]
struct OrderSpec {
    side: Side,
    /// In-range `price_ticks` the JSON price is derived from.
    price_ticks: u64,
    quantity: u32,
    /// `Some(id)` exercises the "preserve supplied id" rule; `None` exercises
    /// the "assign a unique id" rule.
    supplied_id: Option<u64>,
}

fn any_side() -> impl Strategy<Value = Side> {
    prop_oneof![Just(Side::Buy), Just(Side::Sell)]
}

/// An in-range `price_ticks`, spanning the full permitted range including both
/// boundaries (1 → price 0.01 and 99,999,999,999 → price 999,999,999.99).
fn valid_price_ticks() -> impl Strategy<Value = u64> {
    prop_oneof![
        Just(limits::MIN_PRICE_TICKS),
        Just(limits::MAX_PRICE_TICKS),
        limits::MIN_PRICE_TICKS..=limits::MAX_PRICE_TICKS,
    ]
}

/// An in-range integer quantity, including both boundaries (1 and 1,000,000,000).
fn valid_quantity() -> impl Strategy<Value = u32> {
    prop_oneof![
        Just(limits::MIN_GATEWAY_QUANTITY),
        Just(limits::MAX_GATEWAY_QUANTITY),
        limits::MIN_GATEWAY_QUANTITY..=limits::MAX_GATEWAY_QUANTITY,
    ]
}

/// A supplied identifier drawn from a high range so it can never collide with
/// the Gateway's small, monotonically assigned auto-identifiers. This keeps the
/// generated stream free of duplicates by construction (duplicates are covered
/// by a separate property).
fn supplied_id() -> impl Strategy<Value = Option<u64>> {
    prop_oneof![
        Just(None),
        ((1u64 << 40)..=u64::MAX).prop_map(Some),
    ]
}

fn order_spec() -> impl Strategy<Value = OrderSpec> {
    (any_side(), valid_price_ticks(), valid_quantity(), supplied_id()).prop_map(
        |(side, price_ticks, quantity, supplied_id)| OrderSpec {
            side,
            price_ticks,
            quantity,
            supplied_id,
        },
    )
}

/// Build a valid order JSON body for `spec`. The price is `price_ticks / 100`,
/// serialized through `serde_json` so it round-trips to the exact same `f64`
/// the Gateway parses.
fn order_body(spec: &OrderSpec) -> (Vec<u8>, f64) {
    let price = spec.price_ticks as f64 / 100.0;
    let side = match spec.side {
        Side::Buy => "buy",
        Side::Sell => "sell",
    };

    let value = match spec.supplied_id {
        Some(id) => serde_json::json!({
            "order_id": id,
            "side": side,
            "price": price,
            "quantity": spec.quantity,
        }),
        None => serde_json::json!({
            "side": side,
            "price": price,
            "quantity": spec.quantity,
        }),
    };

    (serde_json::to_vec(&value).expect("serialize order JSON"), price)
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    // For any JSON order with a valid side, an in-range price, and an in-range
    // integer quantity -- whether or not it supplies an order identifier -- the
    // Gateway accepts it and yields a ValidatedOrder whose side, price (as
    // ticks), and quantity equal the submitted values; a supplied identifier is
    // preserved unchanged, and an assigned identifier is unique among live
    // orders. The accepted order encodes to a single NewOrder message carrying
    // those same normalized fields.
    #[test]
    fn valid_order_normalization(specs in prop::collection::vec(order_spec(), 1..=32)) {
        let mut gw = Gateway::new();
        // Mirror of the identifiers the Gateway holds as live.
        let mut live_ids: HashSet<u64> = HashSet::new();

        for spec in &specs {
            // Skip a repeated supplied identifier: that is a duplicate, which a
            // separate property covers. Every order we submit here is valid.
            if let Some(id) = spec.supplied_id {
                if live_ids.contains(&id) {
                    continue;
                }
            }

            let (body, price) = order_body(spec);
            let order: ValidatedOrder = gw
                .handle_new_order(&body)
                .expect("a valid order must be accepted");

            // Side and quantity are carried through unchanged.
            prop_assert_eq!(order.side, spec.side);
            prop_assert_eq!(order.quantity, spec.quantity);

            // Price is normalized to ticks as round(price * 100).
            let expected_ticks = (price * 100.0).round() as u64;
            prop_assert_eq!(order.price_ticks, expected_ticks);
            prop_assert_eq!(order.price_ticks, spec.price_ticks);

            match spec.supplied_id {
                // Supplied identifier preserved unchanged.
                Some(id) => prop_assert_eq!(order.order_id, id),
                // Assigned identifier is unique among live orders.
                None => prop_assert!(
                    !live_ids.contains(&order.order_id),
                    "assigned id {} collides with a live order",
                    order.order_id
                ),
            }

            // The identifier is now live in the Gateway.
            prop_assert!(gw.is_in_use(order.order_id));
            prop_assert!(
                live_ids.insert(order.order_id),
                "id {} was assigned twice",
                order.order_id
            );

            // The accepted order produces a single NewOrder message whose side,
            // price ticks, and quantity equal the submitted values.
            match gw.new_order_message(&order) {
                BinaryMessage::NewOrder {
                    order_id,
                    side,
                    price_ticks,
                    quantity,
                    ..
                } => {
                    prop_assert_eq!(order_id, order.order_id);
                    prop_assert_eq!(side, spec.side);
                    prop_assert_eq!(price_ticks, expected_ticks);
                    prop_assert_eq!(quantity, spec.quantity);
                }
                other => prop_assert!(false, "expected NewOrder, got {:?}", other),
            }
        }

        // Every accepted order remains live and uniquely tracked.
        prop_assert_eq!(gw.live_order_count(), live_ids.len());
    }
}
