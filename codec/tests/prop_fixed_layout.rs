// Fixed layout per message type.
//
// For any two messages of the same type, their encoded byte sequences have
// identical total length and identical field layout.
//
// Holds a single property-based test so it composes cleanly with the other
// codec property tests that live in their own files.

use codec::{
    layout, limits, AckKind, BinaryMessage, MessageType, RejectReason, Side,
};
use proptest::prelude::*;

/// In-range `price_ticks` so encode never fails on a field-range check; the
/// fixed-layout property is about valid messages.
fn price_ticks() -> impl Strategy<Value = u64> {
    limits::MIN_PRICE_TICKS..=limits::MAX_PRICE_TICKS
}

/// In-range `quantity` so encode never fails on a field-range check.
fn quantity() -> impl Strategy<Value = u32> {
    limits::MIN_GATEWAY_QUANTITY..=limits::MAX_GATEWAY_QUANTITY
}

fn side() -> impl Strategy<Value = Side> {
    prop_oneof![Just(Side::Buy), Just(Side::Sell)]
}

fn ack_kind() -> impl Strategy<Value = AckKind> {
    prop_oneof![Just(AckKind::Accepted), Just(AckKind::Cancelled)]
}

fn reject_reason() -> impl Strategy<Value = RejectReason> {
    prop_oneof![
        Just(RejectReason::InvalidPrice),
        Just(RejectReason::InvalidQuantity),
        Just(RejectReason::OrderNotFound),
        Just(RejectReason::NoLongerResting),
        Just(RejectReason::IntegrityViolation),
    ]
}

/// Generate an arbitrary, in-range `BinaryMessage` of the given `MessageType`.
/// Field values vary freely across their full permitted ranges so the property
/// genuinely exercises "length depends on type, not field values".
fn message_of(ty: MessageType) -> BoxedStrategy<BinaryMessage> {
    match ty {
        MessageType::NewOrder => (any::<u64>(), side(), price_ticks(), quantity(), any::<u64>())
            .prop_map(|(order_id, side, price_ticks, quantity, seq)| {
                BinaryMessage::NewOrder {
                    order_id,
                    side,
                    price_ticks,
                    quantity,
                    seq,
                }
            })
            .boxed(),
        MessageType::CancelOrder => any::<u64>()
            .prop_map(|order_id| BinaryMessage::CancelOrder { order_id })
            .boxed(),
        MessageType::Trade => (
            any::<u64>(),
            price_ticks(),
            quantity(),
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
            })
            .boxed(),
        MessageType::Ack => (any::<u64>(), ack_kind())
            .prop_map(|(order_id, kind)| BinaryMessage::Ack { order_id, kind })
            .boxed(),
        MessageType::Reject => (any::<u64>(), reject_reason())
            .prop_map(|(order_id, reason)| BinaryMessage::Reject { order_id, reason })
            .boxed(),
    }
}

/// The fixed expected encoded length the wire format mandates for each type.
fn expected_len(ty: MessageType) -> usize {
    match ty {
        MessageType::NewOrder => 30,
        MessageType::CancelOrder => 9,
        MessageType::Trade => 37,
        MessageType::Ack => 10,
        MessageType::Reject => 10,
    }
}

fn message_type() -> impl Strategy<Value = MessageType> {
    prop_oneof![
        Just(MessageType::NewOrder),
        Just(MessageType::CancelOrder),
        Just(MessageType::Trade),
        Just(MessageType::Ack),
        Just(MessageType::Reject),
    ]
}

/// Pick a message type, then generate two independent instances of that type.
fn same_type_pair() -> impl Strategy<Value = (MessageType, BinaryMessage, BinaryMessage)> {
    message_type().prop_flat_map(|ty| {
        (Just(ty), message_of(ty), message_of(ty))
    })
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Every encoded instance of a given message type produces exactly the
    /// fixed byte length for that type, so encoded length depends only on the
    /// message type, not on field values. Two arbitrary instances of the same
    /// type therefore encode to the same fixed length.
    #[test]
    fn fixed_layout_per_message_type((ty, a, b) in same_type_pair()) {
        let want = expected_len(ty);

        // A buffer large enough for any message type.
        let mut buf_a = [0u8; layout::MAX_MESSAGE_LEN];
        let mut buf_b = [0u8; layout::MAX_MESSAGE_LEN];

        let len_a = a.encode(&mut buf_a).expect("in-range message encodes");
        let len_b = b.encode(&mut buf_b).expect("in-range message encodes");

        // Each instance encodes to exactly the fixed length for its type.
        prop_assert_eq!(len_a, want);
        prop_assert_eq!(len_b, want);

        // Length depends only on the type: two arbitrary same-type instances
        // produce identical total length regardless of their field values.
        prop_assert_eq!(len_a, len_b);

        // Both instances also carry the same leading type discriminator byte,
        // confirming the shared field layout begins identically.
        prop_assert_eq!(buf_a[0], ty.as_byte());
        prop_assert_eq!(buf_b[0], ty.as_byte());
    }
}
