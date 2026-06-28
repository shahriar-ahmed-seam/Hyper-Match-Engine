// Shared Wire_Protocol primitives for the Hyper-Match-Engine.
//
// C++ mirror of the Rust `codec` crate's shared types; both sides agree
// byte-for-byte on the wire protocol. Defines the shared enums and the fixed
// message-layout constants.
//
// All multi-byte fields are little-endian, fixed-width integers. Every message
// begins with a single message-type discriminator byte followed by fixed-width
// fields, so every instance of a given message type has an identical layout and
// total byte length.

#ifndef HME_WIRE_PROTOCOL_HPP
#define HME_WIRE_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>

namespace hme {

// ---------------------------------------------------------------------------
// Shared enums (mirror codec/src/lib.rs byte-for-byte).
// ---------------------------------------------------------------------------

// The direction of an Order. Serialized as a single discriminator byte on the
// Wire_Protocol.
enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

// Wire_Protocol discriminator byte for a Side.
constexpr std::uint8_t side_as_byte(Side side) noexcept {
    return static_cast<std::uint8_t>(side);
}

// Parse a Wire_Protocol discriminator byte back into a Side.
constexpr std::optional<Side> side_from_byte(std::uint8_t byte) noexcept {
    switch (byte) {
        case 0: return Side::Buy;
        case 1: return Side::Sell;
        default: return std::nullopt;
    }
}

// The kind of acknowledgement carried by an Ack Binary_Message.
enum class AckKind : std::uint8_t {
    Accepted = 0,   // The order was accepted into the system.
    Cancelled = 1,  // A resting order was cancelled.
};

constexpr std::uint8_t ack_kind_as_byte(AckKind kind) noexcept {
    return static_cast<std::uint8_t>(kind);
}

constexpr std::optional<AckKind> ack_kind_from_byte(std::uint8_t byte) noexcept {
    switch (byte) {
        case 0: return AckKind::Accepted;
        case 1: return AckKind::Cancelled;
        default: return std::nullopt;
    }
}

// The reason carried by a Reject Binary_Message.
enum class RejectReason : std::uint8_t {
    InvalidPrice = 0,        // Price outside the permitted range.
    InvalidQuantity = 1,     // Quantity outside the permitted range.
    OrderNotFound = 2,       // Cancel referenced an absent order.
    NoLongerResting = 3,     // Cancel referenced a non-resting order.
    IntegrityViolation = 4,  // Operation would break a book invariant.
};

constexpr std::uint8_t reject_reason_as_byte(RejectReason reason) noexcept {
    return static_cast<std::uint8_t>(reason);
}

constexpr std::optional<RejectReason> reject_reason_from_byte(std::uint8_t byte) noexcept {
    switch (byte) {
        case 0: return RejectReason::InvalidPrice;
        case 1: return RejectReason::InvalidQuantity;
        case 2: return RejectReason::OrderNotFound;
        case 3: return RejectReason::NoLongerResting;
        case 4: return RejectReason::IntegrityViolation;
        default: return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Wire_Protocol layout constants.
// ---------------------------------------------------------------------------

namespace wire {

// Message-type discriminator bytes (first byte of every Binary_Message).
// Ordering mirrors the BinaryMessage enum and the Rust codec crate so both
// implementations agree byte-for-byte.
enum class MessageType : std::uint8_t {
    NewOrder = 0,
    CancelOrder = 1,
    Trade = 2,
    Ack = 3,
    Reject = 4,
};

constexpr std::uint8_t message_type_as_byte(MessageType type) noexcept {
    return static_cast<std::uint8_t>(type);
}

constexpr std::optional<MessageType> message_type_from_byte(std::uint8_t byte) noexcept {
    switch (byte) {
        case 0: return MessageType::NewOrder;
        case 1: return MessageType::CancelOrder;
        case 2: return MessageType::Trade;
        case 3: return MessageType::Ack;
        case 4: return MessageType::Reject;
        default: return std::nullopt;
    }
}

// Width in bytes of each field used by the Wire_Protocol.
inline constexpr std::size_t kMessageTypeBytes = 1;  // discriminator
inline constexpr std::size_t kOrderIdBytes = 8;      // u64
inline constexpr std::size_t kSideBytes = 1;         // Side discriminator
inline constexpr std::size_t kPriceTicksBytes = 8;   // u64 (price * 100)
inline constexpr std::size_t kQuantityBytes = 4;     // u32
inline constexpr std::size_t kSeqBytes = 8;          // u64 arrival sequence
inline constexpr std::size_t kExecSeqBytes = 8;      // u64 execution sequence
inline constexpr std::size_t kAckKindBytes = 1;      // AckKind discriminator
inline constexpr std::size_t kRejectReasonBytes = 1; // RejectReason discriminator

// Fixed total byte length per message type. All instances of a given type
// share this exact length.
//
//   NewOrder    : type + order_id + side + price_ticks + quantity + seq
//   CancelOrder : type + order_id
//   Trade       : type + exec_seq + price_ticks + quantity + incoming_id + resting_id
//   Ack         : type + order_id + kind
//   Reject      : type + order_id + reason
inline constexpr std::size_t kNewOrderLen =
    kMessageTypeBytes + kOrderIdBytes + kSideBytes + kPriceTicksBytes +
    kQuantityBytes + kSeqBytes;  // 30 bytes
inline constexpr std::size_t kCancelOrderLen =
    kMessageTypeBytes + kOrderIdBytes;  // 9 bytes
inline constexpr std::size_t kTradeLen =
    kMessageTypeBytes + kExecSeqBytes + kPriceTicksBytes + kQuantityBytes +
    kOrderIdBytes + kOrderIdBytes;  // 37 bytes
inline constexpr std::size_t kAckLen =
    kMessageTypeBytes + kOrderIdBytes + kAckKindBytes;  // 10 bytes
inline constexpr std::size_t kRejectLen =
    kMessageTypeBytes + kOrderIdBytes + kRejectReasonBytes;  // 10 bytes

// The largest encoded Binary_Message. Used to size fixed buffers.
inline constexpr std::size_t kMaxMessageLen = kTradeLen;

// Total length in bytes for a given message type, or nullopt for an unknown
// type byte.
constexpr std::optional<std::size_t> message_len(MessageType type) noexcept {
    switch (type) {
        case MessageType::NewOrder: return kNewOrderLen;
        case MessageType::CancelOrder: return kCancelOrderLen;
        case MessageType::Trade: return kTradeLen;
        case MessageType::Ack: return kAckLen;
        case MessageType::Reject: return kRejectLen;
    }
    return std::nullopt;
}

}  // namespace wire

// ---------------------------------------------------------------------------
// Field range constants (Wire_Protocol permitted ranges).
// ---------------------------------------------------------------------------

namespace limits {

// price_ticks = price * 100; JSON price range 0.01 .. 999,999,999.99 maps to
// ticks 1 .. 99,999,999,999.
inline constexpr std::uint64_t kMinPriceTicks = 1;
inline constexpr std::uint64_t kMaxPriceTicks = 99'999'999'999ULL;

// Gateway-side quantity domain.
inline constexpr std::uint32_t kMinGatewayQuantity = 1;
inline constexpr std::uint32_t kMaxGatewayQuantity = 1'000'000'000U;

// Engine-side matching quantity domain.
inline constexpr std::uint32_t kMinEngineQuantity = 1;
inline constexpr std::uint32_t kMaxEngineQuantity = 1'000'000U;

}  // namespace limits

}  // namespace hme

#endif  // HME_WIRE_PROTOCOL_HPP
