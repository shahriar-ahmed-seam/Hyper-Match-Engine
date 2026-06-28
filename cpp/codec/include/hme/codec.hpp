// Binary_Codec encode/decode API for the Hyper-Match-Engine.
//
// Serializes/deserializes Binary_Messages to/from the fixed-layout
// Wire_Protocol. C++ mirror of the Rust `codec` crate's encode/decode; both
// sides agree byte-for-byte.
//
// Every message is `[type:u8][fixed-width little-endian fields...]` with a
// fixed total length per type (see hme::wire). Neither function allocates,
// throws, or reads a clock, so both are safe on the hot path; all error
// conditions are reported through the typed CodecResult/CodecError vocabulary.

#ifndef HME_CODEC_HPP
#define HME_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "hme/binary_message.hpp"

namespace hme {

// Encode `msg` into the caller-provided buffer `out` using the fixed-layout
// Wire_Protocol (little-endian, fixed-width). On success returns the number of
// bytes written (always equal to encoded_len_of(msg)).
//
// Errors (no bytes are written on error):
//   - FieldOutOfRange : a field value is outside its Wire_Protocol range.
//   - InsufficientLength : `out` is smaller than the message's fixed length, so
//                       there is no room to encode the message.
//
// Never allocates, throws, or reads a clock.
CodecResult<std::size_t> encode(const BinaryMessage& msg,
                                std::span<std::uint8_t> out) noexcept;

// Decode a Binary_Message from `bytes`. Validates the leading type byte, the
// exact length for that type, and every field's permitted range.
//
// Errors:
//   - UnknownType        : the leading type byte names no known message type.
//   - InsufficientLength : `bytes` is shorter than the declared type requires;
//                          the caller's state is left untouched so it may wait
//                          for more bytes.
//   - ExcessTrailingBytes: `bytes` is longer than the declared type requires.
//   - FieldOutOfRange    : a decoded field (enum discriminator, price, or
//                          quantity) is outside its permitted range.
//
// Never allocates, throws, or reads a clock.
CodecResult<BinaryMessage> decode(std::span<const std::uint8_t> bytes) noexcept;

}  // namespace hme

#endif  // HME_CODEC_HPP
