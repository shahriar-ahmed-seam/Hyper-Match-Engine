// Codec decode/encode byte round trip.
//
// For any byte sequence that the codec decodes successfully, decoding it and
// then re-encoding the resulting message yields a byte sequence equal to the
// original bytes.
//
// We obtain "byte sequences that decode successfully" by generating valid
// in-range messages spanning every message type and full field ranges
// (including boundaries) and encoding them; the encoded bytes are exactly the
// well-formed wire frames the codec accepts. The property then asserts
// `encode(decode(bytes)) == bytes`.

use codec::{layout, limits, AckKind, BinaryMessage, RejectReason, Side};
use proptest::prelude::*;

/// Strategy for a valid `Side` discriminator.
fn side_strategy() -> impl Strategy<Value = Side> {
    prop_oneof![Just(Side::Buy), Just(Side::Sell)]
}

/// Strategy for a valid `AckKind` discriminator.
fn ack_kind_strategy() -> impl Strategy<Value = AckKind> {
    prop_oneof![Just(AckKind::Accepted), Just(AckKind::Cancelled)]
}

/// Strategy for a valid `RejectReason` discriminator.
fn reject_reason_strategy() -> impl Strategy<Value = RejectReason> {
    prop_oneof![
        Just(RejectReason::InvalidPrice),
        Just(RejectReason::InvalidQuantity),
        Just(RejectReason::OrderNotFound),
        Just(RejectReason::NoLongerResting),
        Just(RejectReason::IntegrityViolation),
    ]
}

/// Strategy for an in-range `price_ticks` value, biased to include both
/// boundaries.
fn price_ticks_strategy() -> impl Strategy<Value = u64> {
    prop_oneof![
        Just(limits::MIN_PRICE_TICKS),
        Just(limits::MAX_PRICE_TICKS),
        limits::MIN_PRICE_TICKS..=limits::MAX_PRICE_TICKS,
    ]
}

/// Strategy for an in-range `quantity` value, biased to include both boundaries.
fn quantity_strategy() -> impl Strategy<Value = u32> {
    prop_oneof![
        Just(limits::MIN_GATEWAY_QUANTITY),
        Just(limits::MAX_GATEWAY_QUANTITY),
        limits::MIN_GATEWAY_QUANTITY..=limits::MAX_GATEWAY_QUANTITY,
    ]
}

/// Strategy producing a valid, in-range `BinaryMessage` of every variant.
fn binary_message_strategy() -> impl Strategy<Value = BinaryMessage> {
    prop_oneof![
        (
            any::<u64>(),
            side_strategy(),
            price_ticks_strategy(),
            quantity_strategy(),
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
            price_ticks_strategy(),
            quantity_strategy(),
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
        (any::<u64>(), ack_kind_strategy())
            .prop_map(|(order_id, kind)| BinaryMessage::Ack { order_id, kind }),
        (any::<u64>(), reject_reason_strategy())
            .prop_map(|(order_id, reason)| BinaryMessage::Reject { order_id, reason }),
    ]
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    #[test]
    fn decode_then_encode_is_a_byte_round_trip(msg in binary_message_strategy()) {
        // Produce a byte sequence the codec decodes successfully by encoding a
        // valid message into an exact-length buffer.
        let mut original = [0u8; layout::MAX_MESSAGE_LEN];
        let len = msg.encode(&mut original).expect("valid message must encode");
        let original = &original[..len];

        // The codec must decode the well-formed frame successfully.
        let decoded = decode_or_panic(original);

        // Re-encoding the decoded message must reproduce the original bytes exactly.
        let mut reencoded = [0u8; layout::MAX_MESSAGE_LEN];
        let reencoded_len = decoded
            .encode(&mut reencoded)
            .expect("decoded message must re-encode");

        prop_assert_eq!(reencoded_len, len, "re-encoded length differs from original");
        prop_assert_eq!(&reencoded[..reencoded_len], original, "re-encoded bytes differ from original");
    }
}

/// Decode a byte sequence that is expected to be a well-formed wire frame,
/// panicking with the codec error if it unexpectedly fails to decode.
fn decode_or_panic(bytes: &[u8]) -> BinaryMessage {
    codec::decode(bytes).unwrap_or_else(|err| {
        panic!("expected bytes to decode successfully, got error: {err}")
    })
}
