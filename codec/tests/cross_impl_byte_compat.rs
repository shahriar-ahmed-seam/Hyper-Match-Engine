// Cross-implementation byte-compatibility test (Rust side).
//
// Asserts the Rust and C++ codecs encode the same messages to byte-identical
// sequences.
//
// Why golden vectors instead of a two-toolchain harness: driving the Rust and
// C++ encoders from one process would require an FFI/IPC bridge and a combined
// Rust+C++ build, which is fragile and slow. Instead we define ONE fixed set of
// representative message vectors together with their EXPECTED golden byte arrays
// (hand-computed from the wire format: a leading little-endian type byte
// followed by little-endian fixed-width fields). This file asserts
// `Rust encode(vector) == golden`. The mirror file
// `cpp/tests/cross_impl_byte_compat_test.cpp` asserts `C++ encode(vector) ==
// the SAME golden bytes`. Because both implementations are checked against the
// identical hand-computed golden vectors, byte-for-byte agreement between the
// two implementations follows transitively (Rust == golden == C++).
//
// The two files MUST be kept in lock step: the vectors and golden bytes here
// are duplicated verbatim in the C++ mirror. Each vector covers a distinct part
// of the layout (every message type, both sides, both ack kinds, several reject
// reasons, and the price/quantity/order-id boundary values).

use codec::{AckKind, BinaryMessage, RejectReason, Side};

/// A fixed message vector paired with its expected golden encoding.
struct Vector {
    name: &'static str,
    msg: BinaryMessage,
    golden: &'static [u8],
}

/// The shared, fixed set of representative vectors. These exact messages and
/// golden bytes are mirrored byte-for-byte in the C++ test.
fn vectors() -> Vec<Vector> {
    vec![
        // 1) NewOrder, minimum in-range values, Buy.
        Vector {
            name: "NewOrder min/Buy",
            msg: BinaryMessage::NewOrder {
                order_id: 1,
                side: Side::Buy,
                price_ticks: 1,
                quantity: 1,
                seq: 0,
            },
            golden: &[
                0x00, // type = NewOrder
                0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // order_id = 1
                0x00, // side = Buy
                0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // price_ticks = 1
                0x01, 0x00, 0x00, 0x00, // quantity = 1
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // seq = 0
            ],
        },
        // 2) NewOrder, mixed multi-byte values, Sell.
        Vector {
            name: "NewOrder mixed/Sell",
            msg: BinaryMessage::NewOrder {
                order_id: 0xDEAD_BEEF,
                side: Side::Sell,
                price_ticks: 12_345,
                quantity: 678,
                seq: 0x0102_0304_0506_0708,
            },
            golden: &[
                0x00, // type = NewOrder
                0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00, // order_id = 0xDEADBEEF
                0x01, // side = Sell
                0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // price_ticks = 12345
                0xA6, 0x02, 0x00, 0x00, // quantity = 678
                0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, // seq = 0x0102030405060708
            ],
        },
        // 3) NewOrder, maximum in-range values, Sell.
        Vector {
            name: "NewOrder max/Sell",
            msg: BinaryMessage::NewOrder {
                order_id: 0xFFFF_FFFF_FFFF_FFFF,
                side: Side::Sell,
                price_ticks: 99_999_999_999, // MAX_PRICE_TICKS
                quantity: 1_000_000_000,     // MAX_GATEWAY_QUANTITY
                seq: 0xFFFF_FFFF_FFFF_FFFF,
            },
            golden: &[
                0x00, // type = NewOrder
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // order_id = u64::MAX
                0x01, // side = Sell
                0xFF, 0xE7, 0x76, 0x48, 0x17, 0x00, 0x00, 0x00, // price_ticks = 99,999,999,999
                0x00, 0xCA, 0x9A, 0x3B, // quantity = 1,000,000,000
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // seq = u64::MAX
            ],
        },
        // 4) CancelOrder, maximum order_id.
        Vector {
            name: "CancelOrder max id",
            msg: BinaryMessage::CancelOrder {
                order_id: 0xFFFF_FFFF_FFFF_FFFF,
            },
            golden: &[
                0x01, // type = CancelOrder
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // order_id = u64::MAX
            ],
        },
        // 5) CancelOrder, mid-range order_id.
        Vector {
            name: "CancelOrder mid id",
            msg: BinaryMessage::CancelOrder {
                order_id: 123_456_789,
            },
            golden: &[
                0x01, // type = CancelOrder
                0x15, 0xCD, 0x5B, 0x07, 0x00, 0x00, 0x00, 0x00, // order_id = 123456789
            ],
        },
        // 6) Trade, mixed values.
        Vector {
            name: "Trade",
            msg: BinaryMessage::Trade {
                exec_seq: 7,
                price_ticks: 50_000,
                quantity: 250,
                incoming_id: 11,
                resting_id: 22,
            },
            golden: &[
                0x02, // type = Trade
                0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // exec_seq = 7
                0x50, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // price_ticks = 50000
                0xFA, 0x00, 0x00, 0x00, // quantity = 250
                0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // incoming_id = 11
                0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // resting_id = 22
            ],
        },
        // 7) Ack, Accepted.
        Vector {
            name: "Ack accepted",
            msg: BinaryMessage::Ack {
                order_id: 5,
                kind: AckKind::Accepted,
            },
            golden: &[
                0x03, // type = Ack
                0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // order_id = 5
                0x00, // kind = Accepted
            ],
        },
        // 8) Ack, Cancelled.
        Vector {
            name: "Ack cancelled",
            msg: BinaryMessage::Ack {
                order_id: 6,
                kind: AckKind::Cancelled,
            },
            golden: &[
                0x03, // type = Ack
                0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // order_id = 6
                0x01, // kind = Cancelled
            ],
        },
        // 9) Reject, NoLongerResting.
        Vector {
            name: "Reject no-longer-resting",
            msg: BinaryMessage::Reject {
                order_id: 8,
                reason: RejectReason::NoLongerResting,
            },
            golden: &[
                0x04, // type = Reject
                0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // order_id = 8
                0x03, // reason = NoLongerResting
            ],
        },
        // 10) Reject, IntegrityViolation.
        Vector {
            name: "Reject integrity-violation",
            msg: BinaryMessage::Reject {
                order_id: 10,
                reason: RejectReason::IntegrityViolation,
            },
            golden: &[
                0x04, // type = Reject
                0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // order_id = 10
                0x04, // reason = IntegrityViolation
            ],
        },
    ]
}

#[test]
fn rust_encode_matches_golden_cross_impl_vectors() {
    let mut buf = [0u8; 64];
    for v in vectors() {
        let written = v
            .msg
            .encode(&mut buf)
            .unwrap_or_else(|e| panic!("encode failed for vector '{}': {e}", v.name));
        assert_eq!(
            &buf[..written],
            v.golden,
            "vector '{}' encoded to bytes that differ from the shared golden vector \
             (the C++ codec is checked against the same golden bytes, so a mismatch \
             here means the two implementations would disagree)",
            v.name
        );
    }
}
