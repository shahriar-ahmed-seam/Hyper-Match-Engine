//! Shared wire-protocol primitives for the Hyper-Match-Engine.
//!
//! Defines the types the Gateway (Rust) and the matching engine (C++) must
//! agree on byte-for-byte: the message enums, the `BinaryMessage` variants, and
//! the encode/decode logic that serializes them to and from the wire.

use serde::{Deserialize, Serialize};

/// The direction of an order.
///
/// Serialized as the lowercase strings `"buy"` / `"sell"` at the JSON boundary
/// and as a single discriminator byte on the wire.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Side {
    Buy,
    Sell,
}

impl Side {
    /// Wire discriminator byte for this side.
    pub const fn as_byte(self) -> u8 {
        match self {
            Side::Buy => 0,
            Side::Sell => 1,
        }
    }

    /// Parse a wire discriminator byte back into a `Side`.
    pub const fn from_byte(byte: u8) -> Option<Side> {
        match byte {
            0 => Some(Side::Buy),
            1 => Some(Side::Sell),
            _ => None,
        }
    }
}

/// The kind of acknowledgement carried by an `Ack` message.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum AckKind {
    /// The order was accepted into the system.
    Accepted,
    /// A resting order was cancelled.
    Cancelled,
}

impl AckKind {
    /// Wire discriminator byte for this acknowledgement kind.
    pub const fn as_byte(self) -> u8 {
        match self {
            AckKind::Accepted => 0,
            AckKind::Cancelled => 1,
        }
    }

    /// Parse a wire discriminator byte back into an `AckKind`.
    pub const fn from_byte(byte: u8) -> Option<AckKind> {
        match byte {
            0 => Some(AckKind::Accepted),
            1 => Some(AckKind::Cancelled),
            _ => None,
        }
    }
}

/// The reason carried by a `Reject` message.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RejectReason {
    /// Order price was outside the permitted range.
    InvalidPrice,
    /// Order quantity was outside the permitted range.
    InvalidQuantity,
    /// A cancel request referenced an order not present in the book.
    OrderNotFound,
    /// A cancel request referenced an order that is no longer resting.
    NoLongerResting,
    /// An operation would have violated a book integrity invariant.
    IntegrityViolation,
}

impl RejectReason {
    /// Wire discriminator byte for this reject reason.
    pub const fn as_byte(self) -> u8 {
        match self {
            RejectReason::InvalidPrice => 0,
            RejectReason::InvalidQuantity => 1,
            RejectReason::OrderNotFound => 2,
            RejectReason::NoLongerResting => 3,
            RejectReason::IntegrityViolation => 4,
        }
    }

    /// Parse a wire discriminator byte back into a `RejectReason`.
    pub const fn from_byte(byte: u8) -> Option<RejectReason> {
        match byte {
            0 => Some(RejectReason::InvalidPrice),
            1 => Some(RejectReason::InvalidQuantity),
            2 => Some(RejectReason::OrderNotFound),
            3 => Some(RejectReason::NoLongerResting),
            4 => Some(RejectReason::IntegrityViolation),
            _ => None,
        }
    }
}

/// The message-type discriminator carried in the first byte of every message.
///
/// The byte values mirror the C++ `hme::wire::MessageType` enum in
/// `cpp/codec/include/hme/wire_protocol.hpp` so both implementations agree
/// byte-for-byte.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum MessageType {
    NewOrder,
    CancelOrder,
    Trade,
    Ack,
    Reject,
}

impl MessageType {
    /// Wire discriminator byte for this message type.
    pub const fn as_byte(self) -> u8 {
        match self {
            MessageType::NewOrder => 0,
            MessageType::CancelOrder => 1,
            MessageType::Trade => 2,
            MessageType::Ack => 3,
            MessageType::Reject => 4,
        }
    }

    /// Parse a wire discriminator byte back into a `MessageType`, returning
    /// `None` for an unknown type byte.
    pub const fn from_byte(byte: u8) -> Option<MessageType> {
        match byte {
            0 => Some(MessageType::NewOrder),
            1 => Some(MessageType::CancelOrder),
            2 => Some(MessageType::Trade),
            3 => Some(MessageType::Ack),
            4 => Some(MessageType::Reject),
            _ => None,
        }
    }

    /// Fixed total encoded byte length for this message type. Every instance of
    /// a given type encodes to exactly this many bytes.
    pub const fn encoded_len(self) -> usize {
        match self {
            MessageType::NewOrder => layout::NEW_ORDER_LEN,
            MessageType::CancelOrder => layout::CANCEL_ORDER_LEN,
            MessageType::Trade => layout::TRADE_LEN,
            MessageType::Ack => layout::ACK_LEN,
            MessageType::Reject => layout::REJECT_LEN,
        }
    }
}

/// Wire field widths and fixed per-message byte lengths.
///
/// All multi-byte fields are little-endian, fixed-width integers. Every message
/// is `[type:u8][fixed-width fields...]`, so every instance of a given message
/// type has an identical layout and total byte length. These constants mirror
/// the C++ `hme::wire` namespace byte-for-byte.
pub mod layout {
    /// Message-type discriminator width (the leading byte of every message).
    pub const MESSAGE_TYPE_BYTES: usize = 1;
    /// `order_id` width (`u64`).
    pub const ORDER_ID_BYTES: usize = 8;
    /// `side` discriminator width.
    pub const SIDE_BYTES: usize = 1;
    /// `price_ticks` width (`u64`, price * 100).
    pub const PRICE_TICKS_BYTES: usize = 8;
    /// `quantity` width (`u32`).
    pub const QUANTITY_BYTES: usize = 4;
    /// `seq` arrival-sequence width (`u64`).
    pub const SEQ_BYTES: usize = 8;
    /// `exec_seq` execution-sequence width (`u64`).
    pub const EXEC_SEQ_BYTES: usize = 8;
    /// `AckKind` discriminator width.
    pub const ACK_KIND_BYTES: usize = 1;
    /// `RejectReason` discriminator width.
    pub const REJECT_REASON_BYTES: usize = 1;

    /// NewOrder: type + order_id + side + price_ticks + quantity + seq = 30 bytes.
    pub const NEW_ORDER_LEN: usize = MESSAGE_TYPE_BYTES
        + ORDER_ID_BYTES
        + SIDE_BYTES
        + PRICE_TICKS_BYTES
        + QUANTITY_BYTES
        + SEQ_BYTES;
    /// CancelOrder: type + order_id = 9 bytes.
    pub const CANCEL_ORDER_LEN: usize = MESSAGE_TYPE_BYTES + ORDER_ID_BYTES;
    /// Trade: type + exec_seq + price_ticks + quantity + incoming_id + resting_id = 37 bytes.
    pub const TRADE_LEN: usize = MESSAGE_TYPE_BYTES
        + EXEC_SEQ_BYTES
        + PRICE_TICKS_BYTES
        + QUANTITY_BYTES
        + ORDER_ID_BYTES
        + ORDER_ID_BYTES;
    /// Ack: type + order_id + kind = 10 bytes.
    pub const ACK_LEN: usize = MESSAGE_TYPE_BYTES + ORDER_ID_BYTES + ACK_KIND_BYTES;
    /// Reject: type + order_id + reason = 10 bytes.
    pub const REJECT_LEN: usize = MESSAGE_TYPE_BYTES + ORDER_ID_BYTES + REJECT_REASON_BYTES;

    /// The largest encoded message length; used to size fixed buffers.
    pub const MAX_MESSAGE_LEN: usize = TRADE_LEN;
}

/// Wire permitted field ranges (mirrors the C++ `hme::limits` namespace).
pub mod limits {
    /// Minimum `price_ticks` (price 0.01 → 1 tick).
    pub const MIN_PRICE_TICKS: u64 = 1;
    /// Maximum `price_ticks` (price 999,999,999.99 → 99,999,999,999 ticks).
    pub const MAX_PRICE_TICKS: u64 = 99_999_999_999;

    /// Minimum Gateway-side quantity.
    pub const MIN_GATEWAY_QUANTITY: u32 = 1;
    /// Maximum Gateway-side quantity.
    pub const MAX_GATEWAY_QUANTITY: u32 = 1_000_000_000;

    /// Minimum engine-side matching quantity.
    pub const MIN_ENGINE_QUANTITY: u32 = 1;
    /// Maximum engine-side matching quantity.
    pub const MAX_ENGINE_QUANTITY: u32 = 1_000_000;
}

/// A single decoded unit of the wire protocol.
///
/// Each variant has a fixed encoded byte length (see [`MessageType::encoded_len`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinaryMessage {
    /// A client order to rest or match (Gateway → Engine).
    NewOrder {
        order_id: u64,
        side: Side,
        price_ticks: u64,
        quantity: u32,
        seq: u64,
    },
    /// A request to cancel a resting order (Gateway → Engine).
    CancelOrder { order_id: u64 },
    /// A reported execution between two orders (Engine → Gateway).
    Trade {
        exec_seq: u64,
        price_ticks: u64,
        quantity: u32,
        incoming_id: u64,
        resting_id: u64,
    },
    /// An acknowledgement of acceptance or cancellation (Engine → Gateway).
    Ack { order_id: u64, kind: AckKind },
    /// A rejection of an order or cancel request (Engine → Gateway).
    Reject {
        order_id: u64,
        reason: RejectReason,
    },
}

impl BinaryMessage {
    /// The wire message type of this message.
    pub const fn message_type(&self) -> MessageType {
        match self {
            BinaryMessage::NewOrder { .. } => MessageType::NewOrder,
            BinaryMessage::CancelOrder { .. } => MessageType::CancelOrder,
            BinaryMessage::Trade { .. } => MessageType::Trade,
            BinaryMessage::Ack { .. } => MessageType::Ack,
            BinaryMessage::Reject { .. } => MessageType::Reject,
        }
    }

    /// The fixed encoded byte length of this message.
    pub const fn encoded_len(&self) -> usize {
        self.message_type().encoded_len()
    }
}

/// An error produced while encoding or decoding a message. These are returned
/// as typed results, never panics.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CodecError {
    /// The leading type byte does not name a known message type.
    UnknownType(u8),
    /// The byte sequence is shorter than its declared type requires; the
    /// codec preserves its prior state.
    InsufficientLength,
    /// The byte sequence is longer than its declared type requires.
    ExcessTrailingBytes,
    /// A field value is outside the range permitted by the wire protocol; the
    /// `&'static str` names the offending field.
    FieldOutOfRange(&'static str),
}

impl core::fmt::Display for CodecError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            CodecError::UnknownType(byte) => {
                write!(f, "unknown message type byte: {byte}")
            }
            CodecError::InsufficientLength => {
                write!(f, "byte sequence shorter than declared message type requires")
            }
            CodecError::ExcessTrailingBytes => {
                write!(f, "byte sequence longer than declared message type requires")
            }
            CodecError::FieldOutOfRange(field) => {
                write!(f, "field out of permitted range: {field}")
            }
        }
    }
}

impl std::error::Error for CodecError {}

// ---------------------------------------------------------------------------
// Encode / decode
//
// The wire format is little-endian, fixed-width, and type-discriminated: the
// first byte names the message type and the remaining fixed-width fields follow
// in declaration order (see the `layout` module). Every instance of a given
// type therefore encodes to exactly `MessageType::encoded_len` bytes. This
// logic mirrors the C++ codec byte-for-byte.
// ---------------------------------------------------------------------------

/// Validate that `price_ticks` lies within the permitted range.
const fn check_price(price_ticks: u64) -> Result<(), CodecError> {
    if price_ticks < limits::MIN_PRICE_TICKS || price_ticks > limits::MAX_PRICE_TICKS {
        Err(CodecError::FieldOutOfRange("price_ticks"))
    } else {
        Ok(())
    }
}

/// Validate that `quantity` lies within the permitted range. The wire-level
/// bound is the broad Gateway quantity range; the narrower engine-side matching
/// range is enforced by the matching engine, not the codec.
const fn check_quantity(quantity: u32) -> Result<(), CodecError> {
    if quantity < limits::MIN_GATEWAY_QUANTITY || quantity > limits::MAX_GATEWAY_QUANTITY {
        Err(CodecError::FieldOutOfRange("quantity"))
    } else {
        Ok(())
    }
}

/// A forward-only little-endian writer over a caller-provided buffer.
///
/// The caller guarantees (via the length check in [`BinaryMessage::encode`])
/// that the buffer holds at least the bytes written here, so the slice copies
/// never go out of bounds.
struct Writer<'a> {
    buf: &'a mut [u8],
    pos: usize,
}

impl<'a> Writer<'a> {
    fn new(buf: &'a mut [u8]) -> Self {
        Writer { buf, pos: 0 }
    }

    fn put_u8(&mut self, value: u8) {
        self.buf[self.pos] = value;
        self.pos += 1;
    }

    fn put_u32(&mut self, value: u32) {
        let bytes = value.to_le_bytes();
        self.buf[self.pos..self.pos + bytes.len()].copy_from_slice(&bytes);
        self.pos += bytes.len();
    }

    fn put_u64(&mut self, value: u64) {
        let bytes = value.to_le_bytes();
        self.buf[self.pos..self.pos + bytes.len()].copy_from_slice(&bytes);
        self.pos += bytes.len();
    }
}

/// A forward-only little-endian reader over a validated byte slice.
///
/// [`decode`] validates the total length before constructing a `Reader`, so the
/// fixed-width reads here always stay in bounds.
struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn new(buf: &'a [u8]) -> Self {
        Reader { buf, pos: 0 }
    }

    fn get_u8(&mut self) -> u8 {
        let value = self.buf[self.pos];
        self.pos += 1;
        value
    }

    fn get_u32(&mut self) -> u32 {
        let mut bytes = [0u8; 4];
        bytes.copy_from_slice(&self.buf[self.pos..self.pos + 4]);
        self.pos += 4;
        u32::from_le_bytes(bytes)
    }

    fn get_u64(&mut self) -> u64 {
        let mut bytes = [0u8; 8];
        bytes.copy_from_slice(&self.buf[self.pos..self.pos + 8]);
        self.pos += 8;
        u64::from_le_bytes(bytes)
    }
}

impl BinaryMessage {
    /// Validate every field of this message against its permitted range. Only
    /// `price_ticks` and `quantity` carry numeric ranges; enum-discriminator
    /// fields are range-safe by construction.
    fn validate_fields(&self) -> Result<(), CodecError> {
        match *self {
            BinaryMessage::NewOrder {
                price_ticks,
                quantity,
                ..
            }
            | BinaryMessage::Trade {
                price_ticks,
                quantity,
                ..
            } => {
                check_price(price_ticks)?;
                check_quantity(quantity)?;
            }
            BinaryMessage::CancelOrder { .. }
            | BinaryMessage::Ack { .. }
            | BinaryMessage::Reject { .. } => {}
        }
        Ok(())
    }

    /// Encode this message into `out` as a little-endian, fixed-width,
    /// type-discriminated byte sequence, returning the number of bytes written
    /// (always [`BinaryMessage::encoded_len`]).
    ///
    /// Errors:
    /// - [`CodecError::FieldOutOfRange`] if any field is outside its permitted
    ///   range; no bytes are written.
    /// - [`CodecError::InsufficientLength`] if `out` is smaller than the fixed
    ///   encoded length for this message type; no bytes are written.
    ///
    /// A buffer larger than the encoded length is accepted; only the leading
    /// `encoded_len` bytes are written and the count returned reflects that.
    pub fn encode(&self, out: &mut [u8]) -> Result<usize, CodecError> {
        // Validate field ranges before touching the buffer.
        self.validate_fields()?;

        let len = self.encoded_len();
        if out.len() < len {
            return Err(CodecError::InsufficientLength);
        }

        let mut w = Writer::new(out);
        w.put_u8(self.message_type().as_byte());
        match *self {
            BinaryMessage::NewOrder {
                order_id,
                side,
                price_ticks,
                quantity,
                seq,
            } => {
                w.put_u64(order_id);
                w.put_u8(side.as_byte());
                w.put_u64(price_ticks);
                w.put_u32(quantity);
                w.put_u64(seq);
            }
            BinaryMessage::CancelOrder { order_id } => {
                w.put_u64(order_id);
            }
            BinaryMessage::Trade {
                exec_seq,
                price_ticks,
                quantity,
                incoming_id,
                resting_id,
            } => {
                w.put_u64(exec_seq);
                w.put_u64(price_ticks);
                w.put_u32(quantity);
                w.put_u64(incoming_id);
                w.put_u64(resting_id);
            }
            BinaryMessage::Ack { order_id, kind } => {
                w.put_u64(order_id);
                w.put_u8(kind.as_byte());
            }
            BinaryMessage::Reject { order_id, reason } => {
                w.put_u64(order_id);
                w.put_u8(reason.as_byte());
            }
        }

        debug_assert_eq!(w.pos, len, "writer wrote a different length than encoded_len");
        Ok(len)
    }
}

/// Decode a single wire byte sequence into a [`BinaryMessage`].
///
/// The leading byte selects the message type; the exact fixed length for that
/// type and every field range are then validated before a message is produced.
///
/// Errors:
/// - [`CodecError::InsufficientLength`] if `bytes` is empty (no type byte) or
///   shorter than the declared type requires. Decoding is a pure function of
///   its input, so no caller state is mutated when this is returned.
/// - [`CodecError::UnknownType`] if the leading byte names no known type.
/// - [`CodecError::ExcessTrailingBytes`] if `bytes` is longer than the declared
///   type requires.
/// - [`CodecError::FieldOutOfRange`] if an enum discriminator is invalid or a
///   numeric field is outside its permitted range.
pub fn decode(bytes: &[u8]) -> Result<BinaryMessage, CodecError> {
    let type_byte = *bytes.first().ok_or(CodecError::InsufficientLength)?;
    let msg_type = MessageType::from_byte(type_byte).ok_or(CodecError::UnknownType(type_byte))?;

    let required = msg_type.encoded_len();
    if bytes.len() < required {
        return Err(CodecError::InsufficientLength);
    }
    if bytes.len() > required {
        return Err(CodecError::ExcessTrailingBytes);
    }

    // Read past the leading type byte; total length is now exactly `required`.
    let mut r = Reader::new(&bytes[layout::MESSAGE_TYPE_BYTES..]);
    let msg = match msg_type {
        MessageType::NewOrder => {
            let order_id = r.get_u64();
            let side = Side::from_byte(r.get_u8()).ok_or(CodecError::FieldOutOfRange("side"))?;
            let price_ticks = r.get_u64();
            let quantity = r.get_u32();
            let seq = r.get_u64();
            check_price(price_ticks)?;
            check_quantity(quantity)?;
            BinaryMessage::NewOrder {
                order_id,
                side,
                price_ticks,
                quantity,
                seq,
            }
        }
        MessageType::CancelOrder => {
            let order_id = r.get_u64();
            BinaryMessage::CancelOrder { order_id }
        }
        MessageType::Trade => {
            let exec_seq = r.get_u64();
            let price_ticks = r.get_u64();
            let quantity = r.get_u32();
            let incoming_id = r.get_u64();
            let resting_id = r.get_u64();
            check_price(price_ticks)?;
            check_quantity(quantity)?;
            BinaryMessage::Trade {
                exec_seq,
                price_ticks,
                quantity,
                incoming_id,
                resting_id,
            }
        }
        MessageType::Ack => {
            let order_id = r.get_u64();
            let kind = AckKind::from_byte(r.get_u8()).ok_or(CodecError::FieldOutOfRange("kind"))?;
            BinaryMessage::Ack { order_id, kind }
        }
        MessageType::Reject => {
            let order_id = r.get_u64();
            let reason =
                RejectReason::from_byte(r.get_u8()).ok_or(CodecError::FieldOutOfRange("reason"))?;
            BinaryMessage::Reject { order_id, reason }
        }
    };

    Ok(msg)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn side_byte_round_trips() {
        for side in [Side::Buy, Side::Sell] {
            assert_eq!(Side::from_byte(side.as_byte()), Some(side));
        }
        assert_eq!(Side::from_byte(2), None);
    }

    #[test]
    fn ack_kind_byte_round_trips() {
        for kind in [AckKind::Accepted, AckKind::Cancelled] {
            assert_eq!(AckKind::from_byte(kind.as_byte()), Some(kind));
        }
        assert_eq!(AckKind::from_byte(7), None);
    }

    #[test]
    fn reject_reason_byte_round_trips() {
        for reason in [
            RejectReason::InvalidPrice,
            RejectReason::InvalidQuantity,
            RejectReason::OrderNotFound,
            RejectReason::NoLongerResting,
            RejectReason::IntegrityViolation,
        ] {
            assert_eq!(RejectReason::from_byte(reason.as_byte()), Some(reason));
        }
        assert_eq!(RejectReason::from_byte(99), None);
    }

    #[test]
    fn side_serializes_to_lowercase_json() {
        assert_eq!(serde_json::to_string(&Side::Buy).unwrap(), "\"buy\"");
        assert_eq!(serde_json::to_string(&Side::Sell).unwrap(), "\"sell\"");
    }

    #[test]
    fn message_type_byte_round_trips() {
        for ty in [
            MessageType::NewOrder,
            MessageType::CancelOrder,
            MessageType::Trade,
            MessageType::Ack,
            MessageType::Reject,
        ] {
            assert_eq!(MessageType::from_byte(ty.as_byte()), Some(ty));
        }
        assert_eq!(MessageType::from_byte(5), None);
        assert_eq!(MessageType::from_byte(255), None);
    }

    #[test]
    fn message_type_bytes_mirror_cpp_discriminators() {
        // Must match cpp/codec/include/hme/wire_protocol.hpp byte-for-byte.
        assert_eq!(MessageType::NewOrder.as_byte(), 0);
        assert_eq!(MessageType::CancelOrder.as_byte(), 1);
        assert_eq!(MessageType::Trade.as_byte(), 2);
        assert_eq!(MessageType::Ack.as_byte(), 3);
        assert_eq!(MessageType::Reject.as_byte(), 4);
    }

    #[test]
    fn fixed_message_lengths_match_cpp() {
        // Byte-for-byte agreement with the C++ wire_protocol.hpp constants.
        assert_eq!(layout::NEW_ORDER_LEN, 30);
        assert_eq!(layout::CANCEL_ORDER_LEN, 9);
        assert_eq!(layout::TRADE_LEN, 37);
        assert_eq!(layout::ACK_LEN, 10);
        assert_eq!(layout::REJECT_LEN, 10);
        assert_eq!(layout::MAX_MESSAGE_LEN, 37);
    }

    #[test]
    fn message_type_encoded_len_matches_layout() {
        assert_eq!(MessageType::NewOrder.encoded_len(), layout::NEW_ORDER_LEN);
        assert_eq!(
            MessageType::CancelOrder.encoded_len(),
            layout::CANCEL_ORDER_LEN
        );
        assert_eq!(MessageType::Trade.encoded_len(), layout::TRADE_LEN);
        assert_eq!(MessageType::Ack.encoded_len(), layout::ACK_LEN);
        assert_eq!(MessageType::Reject.encoded_len(), layout::REJECT_LEN);
    }

    #[test]
    fn binary_message_reports_its_type_and_length() {
        let new_order = BinaryMessage::NewOrder {
            order_id: 1,
            side: Side::Buy,
            price_ticks: 100,
            quantity: 5,
            seq: 42,
        };
        assert_eq!(new_order.message_type(), MessageType::NewOrder);
        assert_eq!(new_order.encoded_len(), layout::NEW_ORDER_LEN);

        let cancel = BinaryMessage::CancelOrder { order_id: 7 };
        assert_eq!(cancel.message_type(), MessageType::CancelOrder);
        assert_eq!(cancel.encoded_len(), layout::CANCEL_ORDER_LEN);

        let trade = BinaryMessage::Trade {
            exec_seq: 3,
            price_ticks: 100,
            quantity: 2,
            incoming_id: 1,
            resting_id: 9,
        };
        assert_eq!(trade.message_type(), MessageType::Trade);
        assert_eq!(trade.encoded_len(), layout::TRADE_LEN);

        let ack = BinaryMessage::Ack {
            order_id: 1,
            kind: AckKind::Accepted,
        };
        assert_eq!(ack.message_type(), MessageType::Ack);
        assert_eq!(ack.encoded_len(), layout::ACK_LEN);

        let reject = BinaryMessage::Reject {
            order_id: 1,
            reason: RejectReason::InvalidPrice,
        };
        assert_eq!(reject.message_type(), MessageType::Reject);
        assert_eq!(reject.encoded_len(), layout::REJECT_LEN);
    }

    #[test]
    fn field_range_constants_match_cpp_limits() {
        assert_eq!(limits::MIN_PRICE_TICKS, 1);
        assert_eq!(limits::MAX_PRICE_TICKS, 99_999_999_999);
        assert_eq!(limits::MIN_GATEWAY_QUANTITY, 1);
        assert_eq!(limits::MAX_GATEWAY_QUANTITY, 1_000_000_000);
        assert_eq!(limits::MIN_ENGINE_QUANTITY, 1);
        assert_eq!(limits::MAX_ENGINE_QUANTITY, 1_000_000);
    }

    #[test]
    fn codec_error_is_displayable() {
        // Exercises the Display/Error impls so the variants are usable as
        // typed results by the encode/decode logic.
        assert!(!CodecError::UnknownType(9).to_string().is_empty());
        assert!(!CodecError::InsufficientLength.to_string().is_empty());
        assert!(!CodecError::ExcessTrailingBytes.to_string().is_empty());
        assert!(CodecError::FieldOutOfRange("price_ticks")
            .to_string()
            .contains("price_ticks"));
    }

    // --- encode / decode ---------------------------------------------------

    /// One representative, in-range message of every type, used by the
    /// example-based round-trip tests below.
    fn sample_messages() -> Vec<BinaryMessage> {
        vec![
            BinaryMessage::NewOrder {
                order_id: 0xDEAD_BEEF,
                side: Side::Buy,
                price_ticks: 12_345,
                quantity: 678,
                seq: 0x0102_0304_0506_0708,
            },
            BinaryMessage::NewOrder {
                order_id: 99,
                side: Side::Sell,
                price_ticks: limits::MAX_PRICE_TICKS,
                quantity: limits::MAX_GATEWAY_QUANTITY,
                seq: 0,
            },
            BinaryMessage::CancelOrder {
                order_id: 0xFFFF_FFFF_FFFF_FFFF,
            },
            BinaryMessage::Trade {
                exec_seq: 7,
                price_ticks: limits::MIN_PRICE_TICKS,
                quantity: 1,
                incoming_id: 11,
                resting_id: 22,
            },
            BinaryMessage::Ack {
                order_id: 5,
                kind: AckKind::Accepted,
            },
            BinaryMessage::Ack {
                order_id: 6,
                kind: AckKind::Cancelled,
            },
            BinaryMessage::Reject {
                order_id: 8,
                reason: RejectReason::NoLongerResting,
            },
        ]
    }

    #[test]
    fn encode_returns_fixed_length_and_writes_type_byte() {
        let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
        for msg in sample_messages() {
            let written = msg.encode(&mut buf).expect("encode");
            assert_eq!(written, msg.encoded_len());
            assert_eq!(buf[0], msg.message_type().as_byte());
        }
    }

    #[test]
    fn encode_then_decode_round_trips_every_type() {
        let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
        for msg in sample_messages() {
            let written = msg.encode(&mut buf).expect("encode");
            let decoded = decode(&buf[..written]).expect("decode");
            assert_eq!(decoded, msg);
        }
    }

    #[test]
    fn new_order_has_expected_little_endian_layout() {
        let msg = BinaryMessage::NewOrder {
            order_id: 0x0807_0605_0403_0201,
            side: Side::Sell,
            price_ticks: 0x0000_0011_1415_1617,
            quantity: 0x2122_2324,
            seq: 0x3031_3233_3435_3637,
        };
        let mut buf = [0u8; layout::NEW_ORDER_LEN];
        msg.encode(&mut buf).expect("encode");
        #[rustfmt::skip]
        let expected: [u8; layout::NEW_ORDER_LEN] = [
            MessageType::NewOrder.as_byte(),
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // order_id LE
            Side::Sell.as_byte(),
            0x17, 0x16, 0x15, 0x14, 0x11, 0x00, 0x00, 0x00, // price_ticks LE
            0x24, 0x23, 0x22, 0x21,                         // quantity LE
            0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31, 0x30, // seq LE
        ];
        assert_eq!(buf, expected);
    }

    #[test]
    fn encode_rejects_buffer_smaller_than_message() {
        let msg = BinaryMessage::CancelOrder { order_id: 1 };
        let mut buf = [0u8; layout::CANCEL_ORDER_LEN - 1];
        assert_eq!(msg.encode(&mut buf), Err(CodecError::InsufficientLength));
        // A larger buffer is accepted; only the leading bytes are used.
        let mut big = [0u8; layout::MAX_MESSAGE_LEN];
        assert_eq!(msg.encode(&mut big), Ok(layout::CANCEL_ORDER_LEN));
    }

    #[test]
    fn encode_rejects_out_of_range_price_and_quantity() {
        let mut buf = [0u8; layout::MAX_MESSAGE_LEN];

        let low_price = BinaryMessage::NewOrder {
            order_id: 1,
            side: Side::Buy,
            price_ticks: limits::MIN_PRICE_TICKS - 1,
            quantity: 1,
            seq: 0,
        };
        assert_eq!(
            low_price.encode(&mut buf),
            Err(CodecError::FieldOutOfRange("price_ticks"))
        );

        let high_price = BinaryMessage::Trade {
            exec_seq: 1,
            price_ticks: limits::MAX_PRICE_TICKS + 1,
            quantity: 1,
            incoming_id: 1,
            resting_id: 2,
        };
        assert_eq!(
            high_price.encode(&mut buf),
            Err(CodecError::FieldOutOfRange("price_ticks"))
        );

        let zero_qty = BinaryMessage::NewOrder {
            order_id: 1,
            side: Side::Buy,
            price_ticks: 100,
            quantity: 0,
            seq: 0,
        };
        assert_eq!(
            zero_qty.encode(&mut buf),
            Err(CodecError::FieldOutOfRange("quantity"))
        );

        let big_qty = BinaryMessage::NewOrder {
            order_id: 1,
            side: Side::Buy,
            price_ticks: 100,
            quantity: limits::MAX_GATEWAY_QUANTITY + 1,
            seq: 0,
        };
        assert_eq!(
            big_qty.encode(&mut buf),
            Err(CodecError::FieldOutOfRange("quantity"))
        );
    }

    #[test]
    fn decode_rejects_empty_input_as_insufficient_length() {
        assert_eq!(decode(&[]), Err(CodecError::InsufficientLength));
    }

    #[test]
    fn decode_rejects_unknown_type_byte() {
        assert_eq!(decode(&[5u8]), Err(CodecError::UnknownType(5)));
        // A full-length buffer with an unknown type byte still reports the type.
        let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
        buf[0] = 200;
        assert_eq!(decode(&buf), Err(CodecError::UnknownType(200)));
    }

    #[test]
    fn decode_rejects_short_and_long_byte_sequences() {
        let msg = BinaryMessage::Trade {
            exec_seq: 1,
            price_ticks: 100,
            quantity: 2,
            incoming_id: 3,
            resting_id: 4,
        };
        let mut buf = [0u8; layout::MAX_MESSAGE_LEN + 1];
        let written = msg.encode(&mut buf).expect("encode");

        // One byte short -> insufficient length.
        assert_eq!(
            decode(&buf[..written - 1]),
            Err(CodecError::InsufficientLength)
        );
        // One byte long -> excess trailing bytes.
        assert_eq!(
            decode(&buf[..written + 1]),
            Err(CodecError::ExcessTrailingBytes)
        );
    }

    #[test]
    fn decode_rejects_invalid_enum_discriminators() {
        // Invalid side byte in an otherwise well-formed NewOrder.
        let mut buf = [0u8; layout::NEW_ORDER_LEN];
        BinaryMessage::NewOrder {
            order_id: 1,
            side: Side::Buy,
            price_ticks: 100,
            quantity: 1,
            seq: 0,
        }
        .encode(&mut buf)
        .expect("encode");
        buf[1 + layout::ORDER_ID_BYTES] = 9; // corrupt side byte
        assert_eq!(decode(&buf), Err(CodecError::FieldOutOfRange("side")));

        // Invalid ack kind byte.
        let mut ack = [0u8; layout::ACK_LEN];
        BinaryMessage::Ack {
            order_id: 1,
            kind: AckKind::Accepted,
        }
        .encode(&mut ack)
        .expect("encode");
        ack[layout::ACK_LEN - 1] = 9;
        assert_eq!(decode(&ack), Err(CodecError::FieldOutOfRange("kind")));

        // Invalid reject reason byte.
        let mut rej = [0u8; layout::REJECT_LEN];
        BinaryMessage::Reject {
            order_id: 1,
            reason: RejectReason::OrderNotFound,
        }
        .encode(&mut rej)
        .expect("encode");
        rej[layout::REJECT_LEN - 1] = 9;
        assert_eq!(decode(&rej), Err(CodecError::FieldOutOfRange("reason")));
    }

    #[test]
    fn decode_rejects_out_of_range_numeric_fields() {
        // Hand-build a NewOrder byte sequence with price_ticks = 0 (below min).
        let mut buf = [0u8; layout::NEW_ORDER_LEN];
        buf[0] = MessageType::NewOrder.as_byte();
        // order_id (8) left as 0
        buf[1 + layout::ORDER_ID_BYTES] = Side::Buy.as_byte();
        // price_ticks (8) left as 0 -> out of range
        // quantity offset:
        let qty_off = 1 + layout::ORDER_ID_BYTES + layout::SIDE_BYTES + layout::PRICE_TICKS_BYTES;
        buf[qty_off..qty_off + 4].copy_from_slice(&1u32.to_le_bytes());
        assert_eq!(decode(&buf), Err(CodecError::FieldOutOfRange("price_ticks")));
    }

    #[test]
    fn decode_then_encode_byte_round_trips() {
        // Bytes produced by encode must re-encode identically after a decode.
        let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
        for msg in sample_messages() {
            let written = msg.encode(&mut buf).expect("encode");
            let original = buf[..written].to_vec();
            let decoded = decode(&original).expect("decode");
            let mut reencoded = [0u8; layout::MAX_MESSAGE_LEN];
            let n = decoded.encode(&mut reencoded).expect("re-encode");
            assert_eq!(&reencoded[..n], &original[..]);
        }
    }
}
