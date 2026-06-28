// Binary_Message structs and typed codec results for the Hyper-Match-Engine.
//
// C++ mirror of the Rust `codec` crate's `BinaryMessage` and `CodecError`
// types; both sides agree byte-for-byte on the wire protocol and on the set of
// message variants and error conditions. Nothing here allocates, throws, or
// reads a clock, so it is safe on the hot path.

#ifndef HME_BINARY_MESSAGE_HPP
#define HME_BINARY_MESSAGE_HPP

#include <cstddef>
#include <cstdint>
#include <variant>

#include "hme/wire_protocol.hpp"

namespace hme {

// ---------------------------------------------------------------------------
// Binary_Message variants (mirror the Rust `BinaryMessage` enum byte-for-byte).
//
//   NewOrder    { order_id, side, price_ticks, quantity, seq }
//   CancelOrder { order_id }
//   Trade       { exec_seq, price_ticks, quantity, incoming_id, resting_id }
//   Ack         { order_id, kind }
//   Reject      { order_id, reason }
//
// Each struct provides defaulted equality so round-trip property tests can
// compare decoded messages against originals.
// ---------------------------------------------------------------------------

// A client instruction to buy or sell that may rest in the Order_Book.
struct NewOrder {
    std::uint64_t order_id = 0;
    Side side = Side::Buy;
    std::uint64_t price_ticks = 0;  // price * 100
    std::uint32_t quantity = 0;
    std::uint64_t seq = 0;  // monotonic arrival sequence for time priority

    friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

// A client instruction to remove a specific Resting_Order from the Order_Book.
struct CancelOrder {
    std::uint64_t order_id = 0;

    friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

// A record produced when an incoming Order matches a Resting_Order.
struct Trade {
    std::uint64_t exec_seq = 0;  // monotonic execution sequence
    std::uint64_t price_ticks = 0;
    std::uint32_t quantity = 0;
    std::uint64_t incoming_id = 0;
    std::uint64_t resting_id = 0;

    friend bool operator==(const Trade&, const Trade&) = default;
};

// An acknowledgement that an order was accepted or a resting order cancelled.
struct Ack {
    std::uint64_t order_id = 0;
    AckKind kind = AckKind::Accepted;

    friend bool operator==(const Ack&, const Ack&) = default;
};

// A rejection carrying the offending order identifier and a machine-readable
// reason.
struct Reject {
    std::uint64_t order_id = 0;
    RejectReason reason = RejectReason::InvalidPrice;

    friend bool operator==(const Reject&, const Reject&) = default;
};

// A single encoded unit of the Wire_Protocol. The alternative ordering matches
// the `wire::MessageType` discriminator ordering (NewOrder, CancelOrder, Trade,
// Ack, Reject) so a message's variant index equals its type byte.
using BinaryMessage =
    std::variant<NewOrder, CancelOrder, Trade, Ack, Reject>;

// The Wire_Protocol discriminator byte for a Binary_Message. The variant
// alternatives are declared in `wire::MessageType` order, so the index is the
// type byte.
constexpr wire::MessageType message_type_of(const BinaryMessage& msg) noexcept {
    return static_cast<wire::MessageType>(msg.index());
}

// The fixed encoded byte length for a Binary_Message's type.
constexpr std::size_t encoded_len_of(const BinaryMessage& msg) noexcept {
    return std::visit(
        [](const auto& m) constexpr noexcept -> std::size_t {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, NewOrder>) {
                return wire::kNewOrderLen;
            } else if constexpr (std::is_same_v<T, CancelOrder>) {
                return wire::kCancelOrderLen;
            } else if constexpr (std::is_same_v<T, Trade>) {
                return wire::kTradeLen;
            } else if constexpr (std::is_same_v<T, Ack>) {
                return wire::kAckLen;
            } else {  // Reject
                return wire::kRejectLen;
            }
        },
        msg);
}

// ---------------------------------------------------------------------------
// CodecError (mirror the Rust `CodecError` enum).
//
//   UnknownType(u8)            -- unrecognized discriminator byte
//   InsufficientLength         -- decoder state preserved for later bytes
//   ExcessTrailingBytes        -- buffer longer than the declared type
//   FieldOutOfRange(field)     -- a field is outside its permitted range
//
// Modeled as a typed value (never an exception) so encode/decode can report
// errors without panics on the hot path.
// ---------------------------------------------------------------------------

enum class CodecErrorKind : std::uint8_t {
    UnknownType = 0,
    InsufficientLength = 1,
    ExcessTrailingBytes = 2,
    FieldOutOfRange = 3,
};

struct CodecError {
    CodecErrorKind kind = CodecErrorKind::InsufficientLength;

    // Valid only when kind == UnknownType: the unrecognized discriminator byte.
    std::uint8_t unknown_type = 0;

    // Valid only when kind == FieldOutOfRange: a static name of the offending
    // field (never owns the string; points at a string literal).
    const char* field = nullptr;

    friend bool operator==(const CodecError&, const CodecError&) = default;

    // ----- Named constructors (one per Rust `CodecError` variant) ----------
    static constexpr CodecError unknown_type_error(std::uint8_t type_byte) noexcept {
        return CodecError{CodecErrorKind::UnknownType, type_byte, nullptr};
    }

    static constexpr CodecError insufficient_length() noexcept {
        return CodecError{CodecErrorKind::InsufficientLength, 0, nullptr};
    }

    static constexpr CodecError excess_trailing_bytes() noexcept {
        return CodecError{CodecErrorKind::ExcessTrailingBytes, 0, nullptr};
    }

    static constexpr CodecError field_out_of_range(const char* field_name) noexcept {
        return CodecError{CodecErrorKind::FieldOutOfRange, 0, field_name};
    }
};

// ---------------------------------------------------------------------------
// CodecResult<T>: a typed success-or-error result with no exceptions.
//
// A minimal, allocation-free `expected`-like type usable on the hot path under
// C++20 (std::expected is C++23). encode returns CodecResult<std::size_t>
// (bytes written); decode returns CodecResult<BinaryMessage>.
// ---------------------------------------------------------------------------

template <typename T>
class CodecResult {
public:
    constexpr CodecResult(T value) noexcept  // NOLINT(google-explicit-constructor)
        : value_(std::move(value)) {}
    constexpr CodecResult(CodecError error) noexcept  // NOLINT(google-explicit-constructor)
        : value_(error) {}

    constexpr bool is_ok() const noexcept {
        return std::holds_alternative<T>(value_);
    }
    constexpr bool is_err() const noexcept { return !is_ok(); }
    constexpr explicit operator bool() const noexcept { return is_ok(); }

    // Precondition: is_ok(). Returns the success value.
    constexpr const T& value() const noexcept { return std::get<T>(value_); }
    constexpr T& value() noexcept { return std::get<T>(value_); }

    // Precondition: is_err(). Returns the error.
    constexpr const CodecError& error() const noexcept {
        return std::get<CodecError>(value_);
    }

    friend bool operator==(const CodecResult&, const CodecResult&) = default;

private:
    std::variant<T, CodecError> value_;
};

}  // namespace hme

#endif  // HME_BINARY_MESSAGE_HPP
