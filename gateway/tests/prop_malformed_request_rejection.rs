//! Property test for the Gateway's rejection of malformed and invalid order
//! requests.
//!
//! For any request body that is not valid JSON, or any order JSON missing a
//! required field, or any order with a side other than buy/sell, a price
//! outside 0.01–999,999,999.99, or a quantity that is not an integer in
//! 1–1,000,000,000, the Gateway responds with HTTP 400 (identifying the
//! offending field where applicable) and forwards no message.

use gateway::{Gateway, GatewayError};
use proptest::prelude::*;
use serde_json::{json, to_vec, Map, Value};

/// Which rejection the Gateway must produce for a generated request body.
#[derive(Debug, Clone, PartialEq, Eq)]
enum ExpectedKind {
    /// Body is not valid JSON (or not a JSON object) → HTTP 400.
    InvalidJson,
    /// A required field is absent, naming the field → HTTP 400.
    Missing(&'static str),
    /// A field is present but invalid, naming the field → HTTP 400.
    Invalid(&'static str),
}

// --- Building blocks for valid filler fields -------------------------------

/// A valid `side` value (so parsing reaches a later, intentionally bad field).
fn valid_side_value() -> impl Strategy<Value = Value> {
    prop_oneof![Just(json!("buy")), Just(json!("sell"))]
}

/// A valid in-range `price` value used as filler.
fn valid_price_value() -> impl Strategy<Value = Value> {
    (0.02f64..=1_000_000.0).prop_map(|p| json!(p))
}

/// A valid in-range integer `quantity` used as filler.
fn valid_qty_value() -> impl Strategy<Value = Value> {
    (1u64..=1_000_000_000).prop_map(|q| json!(q))
}

/// Serialize a `{side, price, quantity}` object to a JSON body.
fn obj_body(side: Value, price: Value, quantity: Value) -> Vec<u8> {
    let mut m = Map::new();
    m.insert("side".to_string(), side);
    m.insert("price".to_string(), price);
    m.insert("quantity".to_string(), quantity);
    to_vec(&Value::Object(m)).expect("serialize JSON object")
}

// --- Category A: not valid JSON --------------------------------------------

fn invalid_json_case() -> impl Strategy<Value = (Vec<u8>, ExpectedKind)> {
    // Valid JSON that is not an object, plus syntactically broken bodies. The
    // Gateway maps both "parse failed" and "parsed to a non-object" to
    // InvalidJson.
    let curated = prop_oneof![
        Just(b"[1, 2, 3]".to_vec()),
        Just(b"\"a bare string\"".to_vec()),
        Just(b"123".to_vec()),
        Just(b"true".to_vec()),
        Just(b"null".to_vec()),
        Just(b"".to_vec()),
        Just(b"{not json".to_vec()),
        Just(b"{\"side\":}".to_vec()),
        Just(b"{\"side\": \"buy\",".to_vec()),
    ];
    // Arbitrary bytes constrained to anything that does NOT parse to a JSON
    // object, so every generated body lands on the InvalidJson path.
    let arbitrary = any::<Vec<u8>>().prop_filter("must not be a JSON object", |b| {
        !serde_json::from_slice::<Value>(b)
            .map(|v| v.is_object())
            .unwrap_or(false)
    });
    prop_oneof![curated, arbitrary].prop_map(|b| (b, ExpectedKind::InvalidJson))
}

// --- Category B: missing required field ------------------------------------

fn missing_field_case() -> impl Strategy<Value = (Vec<u8>, ExpectedKind)> {
    // Build a fully valid object, then drop exactly one required field. Fields
    // are validated in the order side → price → quantity, and every other
    // field is valid, so the named missing field is exactly the dropped one.
    (
        valid_side_value(),
        valid_price_value(),
        valid_qty_value(),
        0usize..3,
    )
        .prop_map(|(side, price, quantity, which)| {
            let field = ["side", "price", "quantity"][which];
            let mut m = Map::new();
            m.insert("side".to_string(), side);
            m.insert("price".to_string(), price);
            m.insert("quantity".to_string(), quantity);
            m.remove(field);
            let body = to_vec(&Value::Object(m)).expect("serialize JSON object");
            (body, ExpectedKind::Missing(field))
        })
}

// --- Category C: invalid field value ---------------------------------------

/// A `side` value that is neither `"buy"` nor `"sell"` (wrong string, wrong
/// case, or a non-string type).
fn bad_side_value() -> impl Strategy<Value = Value> {
    prop_oneof![
        "[a-zA-Z]{0,8}"
            .prop_filter("must not be buy or sell", |s: &String| s != "buy"
                && s != "sell")
            .prop_map(|s| json!(s)),
        Just(json!("BUY")),
        Just(json!("Sell")),
        Just(json!("")),
        Just(json!(1)),
        Just(json!(true)),
        Just(json!([1, 2])),
    ]
}

/// A `price` value outside 0.01–999,999,999.99, or of a non-numeric type.
fn bad_price_value() -> impl Strategy<Value = Value> {
    prop_oneof![
        (0.0f64..0.0099).prop_map(|p| json!(p)),     // below the 0.01 minimum
        (-1.0e6f64..0.0).prop_map(|p| json!(p)),      // negative
        (1.0e9f64..1.0e12).prop_map(|p| json!(p)),    // above the maximum
        Just(json!("not a number")),                  // wrong type
        Just(json!(true)),                            // wrong type
    ]
}

/// A `quantity` value that is not an integer in 1–1,000,000,000.
fn bad_qty_value() -> impl Strategy<Value = Value> {
    prop_oneof![
        Just(json!(0)),                                          // below minimum
        (1_000_000_001u64..=u64::MAX).prop_map(|q| json!(q)),    // above maximum
        (-1_000_000i64..0).prop_map(|q| json!(q)),               // negative
        (0.0001f64..1_000_000.0)
            .prop_filter("non-integer", |f: &f64| f.fract() != 0.0)
            .prop_map(|f| json!(f)),                             // not an integer
        Just(json!("10")),                                       // wrong type
    ]
}

fn invalid_field_case() -> impl Strategy<Value = (Vec<u8>, ExpectedKind)> {
    let bad_side = (bad_side_value(), valid_price_value(), valid_qty_value())
        .prop_map(|(s, p, q)| (obj_body(s, p, q), ExpectedKind::Invalid("side")));
    let bad_price = (valid_side_value(), bad_price_value(), valid_qty_value())
        .prop_map(|(s, p, q)| (obj_body(s, p, q), ExpectedKind::Invalid("price")));
    let bad_qty = (valid_side_value(), valid_price_value(), bad_qty_value())
        .prop_map(|(s, p, q)| (obj_body(s, p, q), ExpectedKind::Invalid("quantity")));
    prop_oneof![bad_side, bad_price, bad_qty]
}

/// Any malformed or invalid request body, paired with the rejection the
/// Gateway must produce for it.
fn invalid_request() -> impl Strategy<Value = (Vec<u8>, ExpectedKind)> {
    prop_oneof![
        invalid_json_case(),
        missing_field_case(),
        invalid_field_case(),
    ]
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    #[test]
    fn malformed_and_invalid_requests_are_rejected_with_400(
        (body, expected) in invalid_request()
    ) {
        let mut gw = Gateway::new();

        // Must return an error (never panic) and forward nothing.
        let err = gw
            .handle_new_order(&body)
            .expect_err("malformed/invalid request must be rejected");

        // Every rejection in this property maps to HTTP 400.
        prop_assert_eq!(err.http_status(), 400);

        // The error variant must match the offending category, naming the
        // field where applicable.
        match expected {
            ExpectedKind::InvalidJson => {
                prop_assert!(matches!(err, GatewayError::InvalidJson(_)),
                    "expected InvalidJson, got {:?}", err);
            }
            ExpectedKind::Missing(field) => {
                prop_assert_eq!(err, GatewayError::MissingField(field.to_string()));
            }
            ExpectedKind::Invalid(field) => {
                prop_assert_eq!(err, GatewayError::InvalidField(field.to_string()));
            }
        }

        // A rejected request reserves no order identifier, so nothing is
        // forwarded to the matching engine.
        prop_assert_eq!(gw.live_order_count(), 0);
    }
}
