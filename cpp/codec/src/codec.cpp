// codec library translation unit.
//
// Implements the encode/decode logic for the Wire_Protocol. Encoding is
// little-endian, fixed-width, and type-discriminated, and agrees byte-for-byte
// with the Rust `codec` crate. Decoding validates the type byte, the exact
// length, and every field range. Nothing here allocates, throws, or reads a
// clock, so both paths are hot-path safe.

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/wire_protocol.hpp"

namespace hme {

// Compile-time confirmation that the fixed per-type lengths match the
// field-by-field layout. If a layout constant is ever changed inconsistently,
// the build fails here.
static_assert(wire::kNewOrderLen == 30, "NewOrder layout must total 30 bytes");
static_assert(wire::kCancelOrderLen == 9, "CancelOrder layout must total 9 bytes");
static_assert(wire::kTradeLen == 37, "Trade layout must total 37 bytes");
static_assert(wire::kAckLen == 10, "Ack layout must total 10 bytes");
static_assert(wire::kRejectLen == 10, "Reject layout must total 10 bytes");
static_assert(wire::kMaxMessageLen == wire::kTradeLen,
              "Trade is the largest message type");

// The BinaryMessage variant alternatives must be declared in the same order as
// the wire::MessageType discriminator so a message's variant index equals its
// type byte.
static_assert(std::variant_size_v<BinaryMessage> == 5,
              "BinaryMessage must have exactly five variants");
static_assert(std::is_same_v<std::variant_alternative_t<0, BinaryMessage>, NewOrder>);
static_assert(std::is_same_v<std::variant_alternative_t<1, BinaryMessage>, CancelOrder>);
static_assert(std::is_same_v<std::variant_alternative_t<2, BinaryMessage>, Trade>);
static_assert(std::is_same_v<std::variant_alternative_t<3, BinaryMessage>, Ack>);
static_assert(std::is_same_v<std::variant_alternative_t<4, BinaryMessage>, Reject>);

// The discriminator byte derived from the variant index matches the explicit
// wire::MessageType byte for every variant.
static_assert(message_type_of(BinaryMessage{NewOrder{}}) == wire::MessageType::NewOrder);
static_assert(message_type_of(BinaryMessage{CancelOrder{}}) == wire::MessageType::CancelOrder);
static_assert(message_type_of(BinaryMessage{Trade{}}) == wire::MessageType::Trade);
static_assert(message_type_of(BinaryMessage{Ack{}}) == wire::MessageType::Ack);
static_assert(message_type_of(BinaryMessage{Reject{}}) == wire::MessageType::Reject);

// Each variant reports its fixed encoded length.
static_assert(encoded_len_of(BinaryMessage{NewOrder{}}) == wire::kNewOrderLen);
static_assert(encoded_len_of(BinaryMessage{CancelOrder{}}) == wire::kCancelOrderLen);
static_assert(encoded_len_of(BinaryMessage{Trade{}}) == wire::kTradeLen);
static_assert(encoded_len_of(BinaryMessage{Ack{}}) == wire::kAckLen);
static_assert(encoded_len_of(BinaryMessage{Reject{}}) == wire::kRejectLen);

// ---------------------------------------------------------------------------
// Little-endian fixed-width cursor helpers.
//
// A tiny write/read cursor keeps the field order in encode and decode in lock
// step. All integers are serialized least-significant-byte first to match the
// Rust `to_le_bytes` / `from_le_bytes` representation byte-for-byte.
// ---------------------------------------------------------------------------

namespace {

// Writes fixed-width little-endian integers into a byte buffer at a moving
// offset. The caller guarantees the buffer is large enough (the encoder checks
// the fixed length up front), so no bounds checks are needed per field.
struct LeWriter {
    std::uint8_t* out;
    std::size_t pos = 0;

    void u8(std::uint8_t v) noexcept { out[pos++] = v; }

    void u32(std::uint32_t v) noexcept {
        out[pos++] = static_cast<std::uint8_t>(v & 0xFF);
        out[pos++] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        out[pos++] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        out[pos++] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    }

    void u64(std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            out[pos++] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        }
    }
};

// Reads fixed-width little-endian integers from a byte buffer at a moving
// offset. The caller verifies the exact length before reading, so each field
// read stays in bounds.
struct LeReader {
    const std::uint8_t* in;
    std::size_t pos = 0;

    std::uint8_t u8() noexcept { return in[pos++]; }

    std::uint32_t u32() noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(in[pos++]) << (8 * i);
        }
        return v;
    }

    std::uint64_t u64() noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(in[pos++]) << (8 * i);
        }
        return v;
    }
};

// Field-range checks shared by encode and decode so both directions accept the
// exact same set of valid messages (keeps the encode/decode round trips total).
constexpr bool price_ticks_in_range(std::uint64_t price_ticks) noexcept {
    return price_ticks >= limits::kMinPriceTicks &&
           price_ticks <= limits::kMaxPriceTicks;
}

constexpr bool quantity_in_range(std::uint32_t quantity) noexcept {
    return quantity >= limits::kMinGatewayQuantity &&
           quantity <= limits::kMaxGatewayQuantity;
}

}  // namespace

CodecResult<std::size_t> encode(const BinaryMessage& msg,
                                std::span<std::uint8_t> out) noexcept {
    const std::size_t needed = encoded_len_of(msg);
    if (out.size() < needed) {
        // No room in the caller's buffer to hold the fixed-length message.
        return CodecError::insufficient_length();
    }

    LeWriter w{out.data()};

    // Leading discriminator byte: the variant index equals the Wire_Protocol
    // type byte.
    w.u8(wire::message_type_as_byte(message_type_of(msg)));

    return std::visit(
        [&w](const auto& m) -> CodecResult<std::size_t> {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, NewOrder>) {
                if (!price_ticks_in_range(m.price_ticks)) {
                    return CodecError::field_out_of_range("price_ticks");
                }
                if (!quantity_in_range(m.quantity)) {
                    return CodecError::field_out_of_range("quantity");
                }
                w.u64(m.order_id);
                w.u8(side_as_byte(m.side));
                w.u64(m.price_ticks);
                w.u32(m.quantity);
                w.u64(m.seq);
            } else if constexpr (std::is_same_v<T, CancelOrder>) {
                w.u64(m.order_id);
            } else if constexpr (std::is_same_v<T, Trade>) {
                if (!price_ticks_in_range(m.price_ticks)) {
                    return CodecError::field_out_of_range("price_ticks");
                }
                if (!quantity_in_range(m.quantity)) {
                    return CodecError::field_out_of_range("quantity");
                }
                w.u64(m.exec_seq);
                w.u64(m.price_ticks);
                w.u32(m.quantity);
                w.u64(m.incoming_id);
                w.u64(m.resting_id);
            } else if constexpr (std::is_same_v<T, Ack>) {
                w.u64(m.order_id);
                w.u8(ack_kind_as_byte(m.kind));
            } else {  // Reject
                w.u64(m.order_id);
                w.u8(reject_reason_as_byte(m.reason));
            }
            return CodecResult<std::size_t>{w.pos};
        },
        msg);
}

CodecResult<BinaryMessage> decode(std::span<const std::uint8_t> bytes) noexcept {
    // Need at least the leading type byte to know what to expect; treat a
    // missing type byte as insufficient length so the caller can wait for more
    // bytes.
    if (bytes.empty()) {
        return CodecError::insufficient_length();
    }

    const std::uint8_t type_byte = bytes[0];
    const auto type = wire::message_type_from_byte(type_byte);
    if (!type) {
        // Unknown discriminator: no message is produced.
        return CodecError::unknown_type_error(type_byte);
    }

    const std::size_t needed = *wire::message_len(*type);
    if (bytes.size() < needed) {
        // Shorter than the declared type requires; preserve caller state.
        return CodecError::insufficient_length();
    }
    if (bytes.size() > needed) {
        // Longer than the declared type requires.
        return CodecError::excess_trailing_bytes();
    }

    LeReader r{bytes.data()};
    r.u8();  // consume the already-validated type byte

    switch (*type) {
        case wire::MessageType::NewOrder: {
            const std::uint64_t order_id = r.u64();
            const auto side = side_from_byte(r.u8());
            if (!side) {
                return CodecError::field_out_of_range("side");
            }
            const std::uint64_t price_ticks = r.u64();
            const std::uint32_t quantity = r.u32();
            const std::uint64_t seq = r.u64();
            if (!price_ticks_in_range(price_ticks)) {
                return CodecError::field_out_of_range("price_ticks");
            }
            if (!quantity_in_range(quantity)) {
                return CodecError::field_out_of_range("quantity");
            }
            return CodecResult<BinaryMessage>{BinaryMessage{
                NewOrder{order_id, *side, price_ticks, quantity, seq}}};
        }
        case wire::MessageType::CancelOrder: {
            const std::uint64_t order_id = r.u64();
            return CodecResult<BinaryMessage>{
                BinaryMessage{CancelOrder{order_id}}};
        }
        case wire::MessageType::Trade: {
            const std::uint64_t exec_seq = r.u64();
            const std::uint64_t price_ticks = r.u64();
            const std::uint32_t quantity = r.u32();
            const std::uint64_t incoming_id = r.u64();
            const std::uint64_t resting_id = r.u64();
            if (!price_ticks_in_range(price_ticks)) {
                return CodecError::field_out_of_range("price_ticks");
            }
            if (!quantity_in_range(quantity)) {
                return CodecError::field_out_of_range("quantity");
            }
            return CodecResult<BinaryMessage>{BinaryMessage{Trade{
                exec_seq, price_ticks, quantity, incoming_id, resting_id}}};
        }
        case wire::MessageType::Ack: {
            const std::uint64_t order_id = r.u64();
            const auto kind = ack_kind_from_byte(r.u8());
            if (!kind) {
                return CodecError::field_out_of_range("kind");
            }
            return CodecResult<BinaryMessage>{
                BinaryMessage{Ack{order_id, *kind}}};
        }
        case wire::MessageType::Reject: {
            const std::uint64_t order_id = r.u64();
            const auto reason = reject_reason_from_byte(r.u8());
            if (!reason) {
                return CodecError::field_out_of_range("reason");
            }
            return CodecResult<BinaryMessage>{
                BinaryMessage{Reject{order_id, *reason}}};
        }
    }

    // Unreachable: every MessageType is handled above and unknown bytes were
    // rejected earlier. Reported as an unknown type rather than a panic to keep
    // the hot path exception-free.
    return CodecError::unknown_type_error(type_byte);
}

}  // namespace hme
