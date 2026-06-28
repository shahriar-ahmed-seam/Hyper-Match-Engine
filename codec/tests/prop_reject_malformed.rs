// Codec rejects malformed input.
//
// For any byte sequence that declares an unknown message type, is shorter than
// its declared type requires, or carries excess trailing bytes, decoding
// returns the corresponding error and no message (and on insufficient length
// the codec state is preserved); and for any message containing a field outside
// its permitted range, encoding returns an out-of-range error and no byte
// sequence.
//
// Implemented as a single proptest property that generates malformed inputs
// across every rejection category and asserts the codec returns the appropriate
// typed error without ever panicking.

use codec::{decode, AckKind, BinaryMessage, CodecError, MessageType, RejectReason, Side, layout, limits};
use proptest::prelude::*;

/// The error category a generated malformed input is expected to produce. We
/// compare categories (not exact payloads) because the type byte / field name
/// carried by some variants is incidental to the rejection behaviour.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ExpectedErr {
    UnknownType,
    InsufficientLength,
    ExcessTrailingBytes,
    FieldOutOfRange,
}

fn category(err: CodecError) -> ExpectedErr {
    match err {
        CodecError::UnknownType(_) => ExpectedErr::UnknownType,
        CodecError::InsufficientLength => ExpectedErr::InsufficientLength,
        CodecError::ExcessTrailingBytes => ExpectedErr::ExcessTrailingBytes,
        CodecError::FieldOutOfRange(_) => ExpectedErr::FieldOutOfRange,
    }
}

/// A single malformed-input case: either a byte sequence that `decode` must
/// reject with `expected`, or a `BinaryMessage` that `encode` must reject as
/// out-of-range.
#[derive(Debug, Clone)]
enum Case {
    Decode {
        bytes: Vec<u8>,
        expected: ExpectedErr,
    },
    EncodeOutOfRange {
        msg: BinaryMessage,
    },
}

const VALID_TYPE_BYTES: [u8; 5] = [0, 1, 2, 3, 4];

fn required_len(type_byte: u8) -> usize {
    MessageType::from_byte(type_byte)
        .expect("valid type byte")
        .encoded_len()
}

/// A byte sequence whose leading byte names no known message type. The type
/// byte is validated before any length check, so any trailing content must
/// still yield `UnknownType`.
fn unknown_type() -> impl Strategy<Value = Case> {
    (5u8..=255u8, prop::collection::vec(any::<u8>(), 0..40)).prop_map(|(ty, rest)| {
        let mut bytes = Vec::with_capacity(1 + rest.len());
        bytes.push(ty);
        bytes.extend_from_slice(&rest);
        Case::Decode {
            bytes,
            expected: ExpectedErr::UnknownType,
        }
    })
}

/// A known type whose byte sequence is shorter than the type requires
/// (including the degenerate single type byte). Decoding is a pure function of
/// its input, so returning the error leaves all caller state untouched.
fn too_short() -> impl Strategy<Value = Case> {
    prop::sample::select(VALID_TYPE_BYTES.to_vec())
        .prop_flat_map(|ty| {
            let required = required_len(ty);
            // total length in [1, required) => strictly shorter than required.
            (Just(ty), 1usize..required)
        })
        .prop_flat_map(|(ty, len)| {
            prop::collection::vec(any::<u8>(), len - 1).prop_map(move |rest| {
                let mut bytes = Vec::with_capacity(len);
                bytes.push(ty);
                bytes.extend_from_slice(&rest);
                Case::Decode {
                    bytes,
                    expected: ExpectedErr::InsufficientLength,
                }
            })
        })
}

/// A known type whose byte sequence is longer than the type requires.
fn too_long() -> impl Strategy<Value = Case> {
    prop::sample::select(VALID_TYPE_BYTES.to_vec())
        .prop_flat_map(|ty| {
            let required = required_len(ty);
            (Just(ty), (required + 1)..(required + 65))
        })
        .prop_flat_map(|(ty, len)| {
            prop::collection::vec(any::<u8>(), len - 1).prop_map(move |rest| {
                let mut bytes = Vec::with_capacity(len);
                bytes.push(ty);
                bytes.extend_from_slice(&rest);
                Case::Decode {
                    bytes,
                    expected: ExpectedErr::ExcessTrailingBytes,
                }
            })
        })
}

/// An exact-length byte sequence whose enum discriminator (side / ack-kind /
/// reject-reason) is not a defined value. The discriminator is validated as it
/// is read, so decoding rejects with a field-out-of-range error and produces no
/// message.
fn invalid_discriminator() -> impl Strategy<Value = Case> {
    // The side/kind/reason discriminator always lives at offset 9
    // (type byte + 8-byte order_id) in NewOrder, Ack, and Reject.
    const DISC_OFFSET: usize = layout::MESSAGE_TYPE_BYTES + layout::ORDER_ID_BYTES;

    let new_order_bad_side = (any::<u64>(), 2u8..=255u8, any::<u64>()).prop_map(
        move |(order_id, bad_side, seq)| {
            let base = BinaryMessage::NewOrder {
                order_id,
                side: Side::Buy,
                price_ticks: 100,
                quantity: 1,
                seq,
            };
            let mut bytes = vec![0u8; layout::NEW_ORDER_LEN];
            base.encode(&mut bytes).expect("base NewOrder encodes");
            bytes[DISC_OFFSET] = bad_side;
            Case::Decode {
                bytes,
                expected: ExpectedErr::FieldOutOfRange,
            }
        },
    );

    let ack_bad_kind = (any::<u64>(), 2u8..=255u8).prop_map(move |(order_id, bad_kind)| {
        let base = BinaryMessage::Ack {
            order_id,
            kind: AckKind::Accepted,
        };
        let mut bytes = vec![0u8; layout::ACK_LEN];
        base.encode(&mut bytes).expect("base Ack encodes");
        bytes[DISC_OFFSET] = bad_kind;
        Case::Decode {
            bytes,
            expected: ExpectedErr::FieldOutOfRange,
        }
    });

    let reject_bad_reason = (any::<u64>(), 5u8..=255u8).prop_map(move |(order_id, bad_reason)| {
        let base = BinaryMessage::Reject {
            order_id,
            reason: RejectReason::InvalidPrice,
        };
        let mut bytes = vec![0u8; layout::REJECT_LEN];
        base.encode(&mut bytes).expect("base Reject encodes");
        bytes[DISC_OFFSET] = bad_reason;
        Case::Decode {
            bytes,
            expected: ExpectedErr::FieldOutOfRange,
        }
    });

    prop_oneof![new_order_bad_side, ack_bad_kind, reject_bad_reason]
}

/// A message carrying a numeric field (`price_ticks` or `quantity`) outside its
/// permitted range. Encoding must reject it with a field-out-of-range error and
/// write no bytes.
fn encode_out_of_range() -> impl Strategy<Value = Case> {
    let bad_price = prop_oneof![Just(0u64), (limits::MAX_PRICE_TICKS + 1)..=u64::MAX];
    let bad_qty = prop_oneof![Just(0u32), (limits::MAX_GATEWAY_QUANTITY + 1)..=u32::MAX];
    let ok_price = limits::MIN_PRICE_TICKS..=limits::MAX_PRICE_TICKS;
    let ok_qty = limits::MIN_GATEWAY_QUANTITY..=limits::MAX_GATEWAY_QUANTITY;

    (
        any::<bool>(), // is_trade
        any::<bool>(), // corrupt price (else corrupt quantity)
        bad_price,
        bad_qty,
        ok_price,
        ok_qty,
        any::<u64>(),
        any::<u64>(),
    )
        .prop_map(
            |(is_trade, corrupt_price, bp, bq, op, oq, id_a, id_b)| {
                let (price_ticks, quantity) = if corrupt_price { (bp, oq) } else { (op, bq) };
                let msg = if is_trade {
                    BinaryMessage::Trade {
                        exec_seq: id_a,
                        price_ticks,
                        quantity,
                        incoming_id: id_b,
                        resting_id: id_a,
                    }
                } else {
                    BinaryMessage::NewOrder {
                        order_id: id_a,
                        side: Side::Buy,
                        price_ticks,
                        quantity,
                        seq: id_b,
                    }
                };
                Case::EncodeOutOfRange { msg }
            },
        )
}

fn malformed_case() -> impl Strategy<Value = Case> {
    prop_oneof![
        unknown_type(),
        too_short(),
        too_long(),
        invalid_discriminator(),
        encode_out_of_range(),
    ]
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    #[test]
    fn property_7_codec_rejects_malformed_input(case in malformed_case()) {
        match case {
            Case::Decode { bytes, expected } => {
                // decode must return a typed error, never panic.
                match decode(&bytes) {
                    Err(err) => prop_assert_eq!(
                        category(err),
                        expected,
                        "decode({:?}) returned {:?}, expected {:?}",
                        bytes,
                        err,
                        expected
                    ),
                    Ok(msg) => prop_assert!(
                        false,
                        "decode({:?}) unexpectedly produced {:?} (expected {:?})",
                        bytes,
                        msg,
                        expected
                    ),
                }
            }
            Case::EncodeOutOfRange { msg } => {
                let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
                let result = msg.encode(&mut buf);
                prop_assert!(
                    matches!(result, Err(CodecError::FieldOutOfRange(_))),
                    "encode({:?}) returned {:?}, expected FieldOutOfRange",
                    msg,
                    result
                );
            }
        }
    }
}
