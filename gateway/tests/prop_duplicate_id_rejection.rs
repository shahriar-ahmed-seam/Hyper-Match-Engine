//! Property-based test for the Gateway's duplicate order-identifier handling.
//!
//! For any order identifier already in use, submitting a new order with that
//! identifier causes the Gateway to respond with HTTP 409 (duplicate) and to
//! forward no message to the matching engine.

use gateway::{Gateway, GatewayError};
use proptest::prelude::*;

/// Generate a valid JSON order body that supplies an explicit `order_id`.
///
/// Side, price, and quantity are constrained to the Gateway's accepted input
/// space so the *first* submission of the body always validates; the property
/// under test concerns what happens on the *second* (duplicate) submission.
fn duplicate_order_body() -> impl Strategy<Value = (u64, String)> {
    (
        any::<u64>(),
        // Whole-dollar component of the limit price, 0..=999_999_999.
        0u64..=999_999_999u64,
        // Cents component, 0..=99.
        0u64..=99u64,
        // Quantity within the Gateway's accepted range, 1..=1_000_000_000.
        1u32..=1_000_000_000u32,
        prop::bool::ANY,
    )
        // Exclude the single sub-minimum price 0.00 (below 0.01).
        .prop_filter("price must be >= 0.01", |(_, dollars, cents, _, _)| {
            *dollars != 0 || *cents != 0
        })
        .prop_map(|(order_id, dollars, cents, quantity, is_buy)| {
            let side = if is_buy { "buy" } else { "sell" };
            let body = format!(
                r#"{{"order_id": {order_id}, "side": "{side}", "price": {dollars}.{cents:02}, "quantity": {quantity}}}"#
            );
            (order_id, body)
        })
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    #[test]
    fn duplicate_supplied_id_is_rejected_leaving_live_set_unchanged(
        (order_id, body) in duplicate_order_body()
    ) {
        let mut gw = Gateway::new();

        // First submission of a fresh identifier succeeds and reserves it.
        let first = gw.handle_new_order(body.as_bytes());
        prop_assert!(first.is_ok(), "first submission must succeed: {first:?}");
        let accepted = first.unwrap();
        prop_assert_eq!(accepted.order_id, order_id);
        prop_assert!(gw.is_in_use(order_id));

        // Snapshot the live-order set so we can prove the rejection is inert.
        let live_count_before = gw.live_order_count();
        prop_assert_eq!(live_count_before, 1);

        // Second submission with the same identifier is rejected as a duplicate.
        let second = gw.handle_new_order(body.as_bytes());
        match second {
            Err(GatewayError::DuplicateOrderId(id)) => {
                prop_assert_eq!(id, order_id);
                // Maps to HTTP 409 and forwards no message.
                prop_assert_eq!(GatewayError::DuplicateOrderId(id).http_status(), 409);
            }
            other => prop_assert!(
                false,
                "expected DuplicateOrderId({order_id}), got {other:?}"
            ),
        }

        // The rejected duplicate must not disturb the live-order set.
        prop_assert_eq!(gw.live_order_count(), live_count_before);
        prop_assert!(gw.is_in_use(order_id));
    }
}
