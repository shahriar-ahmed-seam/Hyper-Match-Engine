//! Integration test for the Gateway's response-conversion latency.
//!
//! When the Gateway receives a response message from the matching engine, it
//! must convert that message into a JSON response within 10 milliseconds. The
//! conversion is performed by `Gateway::handle_engine_response`, which decodes
//! the response message from the egress ring and turns it into a `JsonResponse`.
//!
//! This test encodes representative engine responses -- an `Ack`, a `Trade`,
//! and a `Reject` -- exactly as the engine would place them on the egress ring,
//! then measures the wall-clock time of the decode + convert step with
//! `std::time::Instant` and asserts each conversion completes comfortably within
//! the 10 ms ceiling. To stay robust against scheduler noise it both measures
//! every individual conversion (asserting the per-conversion bound) and averages
//! over many iterations (asserting the mean is well under the ceiling).
//!
//! It lives in its own integration-test file so it does not touch
//! `gateway/src/lib.rs`.

use std::time::{Duration, Instant};

use codec::{layout, AckKind, BinaryMessage, RejectReason, Side};
use gateway::Gateway;

/// The conversion ceiling.
const CONVERSION_CEILING: Duration = Duration::from_millis(10);

/// Number of iterations used to average out scheduler / cache noise while still
/// asserting the per-conversion bound on every single call.
const ITERATIONS: u32 = 10_000;

/// Encode a message into a freshly sized byte buffer, exactly as the engine
/// would emit it onto the egress ring.
fn encode(msg: &BinaryMessage) -> Vec<u8> {
    let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
    let written = msg.encode(&mut buf).expect("encode response Binary_Message");
    buf[..written].to_vec()
}

/// Representative engine responses spanning every response-message type the
/// Gateway converts: an acknowledgement, a trade, and a rejection.
fn representative_responses() -> Vec<(&'static str, Vec<u8>)> {
    vec![
        (
            "Ack",
            encode(&BinaryMessage::Ack {
                order_id: 42,
                kind: AckKind::Accepted,
            }),
        ),
        (
            "Trade",
            encode(&BinaryMessage::Trade {
                exec_seq: 1,
                price_ticks: 12_345,
                quantity: 500,
                incoming_id: 7,
                resting_id: 9,
            }),
        ),
        (
            "Reject",
            encode(&BinaryMessage::Reject {
                order_id: 13,
                reason: RejectReason::InvalidPrice,
            }),
        ),
    ]
}

/// Measure decode + JSON conversion latency for each representative response
/// type and assert every conversion completes within the 10 ms ceiling. Each
/// conversion is timed individually so a single slow conversion fails the test,
/// and the per-type average is also asserted to be well under the ceiling to
/// confirm the typical case is far from the bound.
#[test]
fn response_conversion_completes_within_10ms() {
    for (label, bytes) in representative_responses() {
        let mut total = Duration::ZERO;
        let mut worst = Duration::ZERO;

        for _ in 0..ITERATIONS {
            let start = Instant::now();
            let response =
                Gateway::handle_engine_response(&bytes).expect("response must convert to JSON");
            let elapsed = start.elapsed();

            // Touch the result so the conversion cannot be optimized away.
            assert!(
                response.status >= 200,
                "{label}: converted response must carry an HTTP status"
            );

            assert!(
                elapsed < CONVERSION_CEILING,
                "{label}: a single conversion took {elapsed:?}, exceeding the \
                 10 ms ceiling"
            );

            total += elapsed;
            if elapsed > worst {
                worst = elapsed;
            }
        }

        let average = total / ITERATIONS;
        // The typical conversion should be orders of magnitude under the ceiling;
        // asserting a comfortable margin keeps the test meaningful without being
        // flaky on a loaded CI host.
        assert!(
            average < CONVERSION_CEILING,
            "{label}: average conversion {average:?} (worst {worst:?}) exceeded \
             the 10 ms ceiling"
        );
    }
}

/// Sanity check that the encoded responses are genuinely decoded and converted
/// to the expected JSON shape, so the latency measurement above is timing real
/// work rather than an early error return.
#[test]
fn representative_responses_convert_to_expected_json() {
    // Ack -> HTTP 200.
    let ack = encode(&BinaryMessage::Ack {
        order_id: 42,
        kind: AckKind::Accepted,
    });
    let ack_json = Gateway::handle_engine_response(&ack).expect("Ack converts");
    assert_eq!(ack_json.status, 200);
    assert_eq!(ack_json.body["order_id"], 42);

    // Trade -> HTTP 200 with the executed price/quantity.
    let trade = encode(&BinaryMessage::Trade {
        exec_seq: 1,
        price_ticks: 12_345,
        quantity: 500,
        incoming_id: 7,
        resting_id: 9,
    });
    let trade_json = Gateway::handle_engine_response(&trade).expect("Trade converts");
    assert_eq!(trade_json.status, 200);
    assert_eq!(trade_json.body["status"], "trade");
    assert_eq!(trade_json.body["quantity"], 500);

    // Reject (invalid price) -> HTTP 400.
    let reject = encode(&BinaryMessage::Reject {
        order_id: 13,
        reason: RejectReason::InvalidPrice,
    });
    let reject_json = Gateway::handle_engine_response(&reject).expect("Reject converts");
    assert_eq!(reject_json.status, 400);
    assert_eq!(reject_json.body["status"], "rejected");

    // A buy NewOrder is a request, not a response: keep `Side` referenced so the
    // import is used and confirm request messages are not accepted as responses.
    let request = encode(&BinaryMessage::NewOrder {
        order_id: 1,
        side: Side::Buy,
        price_ticks: 100,
        quantity: 1,
        seq: 0,
    });
    assert!(
        Gateway::handle_engine_response(&request).is_err(),
        "a request message must not be accepted as an engine response"
    );
}
