//! Property-based test for the codec encode/decode round trip.
//!
//! For any valid message, encoding it and then decoding the resulting byte
//! sequence yields a message equal to the original. Lives in its own
//! integration-test file (one property per test) to avoid conflicts with other
//! test files.

use codec::{decode, layout, limits, AckKind, BinaryMessage, RejectReason, Side};
use proptest::prelude::*;

/// Strategy producing a wire-valid `price_ticks`, spanning the full permitted
/// range including both boundaries (1 and 99,999,999,999).
fn valid_price_ticks() -> impl Strategy<Value = u64> {
    limits::MIN_PRICE_TICKS..=limits::MAX_PRICE_TICKS
}

/// Strategy producing a wire-valid `quantity`, spanning the full permitted
/// range including both boundaries (1 and 1,000,000,000).
fn valid_quantity() -> impl Strategy<Value = u32> {
    limits::MIN_GATEWAY_QUANTITY..=limits::MAX_GATEWAY_QUANTITY
}

fn any_side() -> impl Strategy<Value = Side> {
    prop_oneof![Just(Side::Buy), Just(Side::Sell)]
}

fn any_ack_kind() -> impl Strategy<Value = AckKind> {
    prop_oneof![Just(AckKind::Accepted), Just(AckKind::Cancelled)]
}

fn any_reject_reason() -> impl Strategy<Value = RejectReason> {
    prop_oneof![
        Just(RejectReason::InvalidPrice),
        Just(RejectReason::InvalidQuantity),
        Just(RejectReason::OrderNotFound),
        Just(RejectReason::NoLongerResting),
        Just(RejectReason::IntegrityViolation),
    ]
}

/// Strategy generating any valid `BinaryMessage`, covering every message type
/// with full field ranges (price/quantity constrained to their permitted wire
/// ranges; other integer fields span the entire `u64`/`u32`).
fn any_valid_binary_message() -> impl Strategy<Value = BinaryMessage> {
    prop_oneof![
        (
            any::<u64>(),
            any_side(),
            valid_price_ticks(),
            valid_quantity(),
            any::<u64>(),
        )
            .prop_map(|(order_id, side, price_ticks, quantity, seq)| {
                BinaryMessage::NewOrder {
                    order_id,
                    side,
                    price_ticks,
                    quantity,
                    seq,
                }
            }),
        any::<u64>().prop_map(|order_id| BinaryMessage::CancelOrder { order_id }),
        (
            any::<u64>(),
            valid_price_ticks(),
            valid_quantity(),
            any::<u64>(),
            any::<u64>(),
        )
            .prop_map(|(exec_seq, price_ticks, quantity, incoming_id, resting_id)| {
                BinaryMessage::Trade {
                    exec_seq,
                    price_ticks,
                    quantity,
                    incoming_id,
                    resting_id,
                }
            }),
        (any::<u64>(), any_ack_kind())
            .prop_map(|(order_id, kind)| BinaryMessage::Ack { order_id, kind }),
        (any::<u64>(), any_reject_reason())
            .prop_map(|(order_id, reason)| BinaryMessage::Reject { order_id, reason }),
    ]
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    // For any valid message, encoding it and then decoding the resulting byte
    // sequence yields a message equal to the original, with no loss of any
    // field value.
    #[test]
    fn codec_encode_decode_round_trip(msg in any_valid_binary_message()) {
        let mut buf = [0u8; layout::MAX_MESSAGE_LEN];

        let written = msg
            .encode(&mut buf)
            .expect("a valid message must encode successfully");
        prop_assert_eq!(written, msg.encoded_len());

        let decoded = decode(&buf[..written]).expect("freshly encoded bytes must decode");
        prop_assert_eq!(decoded, msg);
    }
}
