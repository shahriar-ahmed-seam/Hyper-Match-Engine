// Per-connection framing buffer for the Network_Server.
//
// The Network_Server reads raw bytes from a client socket in arbitrary chunks
// (a single socket read can split a Binary_Message, or carry several). This
// component reassembles that byte stream into whole Binary_Messages so the
// consumer (Gateway) never sees a partial frame.
//
// Framing is driven entirely by the fixed-layout Wire_Protocol: the leading
// byte of every Binary_Message is its type discriminator, and each type has a
// fixed total length (see hme::wire::message_len). So the framer reads the type
// byte, looks up the per-type length L, and delivers the next L bytes as one
// decoded Binary_Message, retaining any leftover partial bytes for the next
// read.
//
// The per-connection buffer never exceeds the maximum Binary_Message size of
// kMaxMessageSize (65,536) bytes. If a frame cannot be decoded -- an unknown
// type byte, a declared length larger than the cap, or a field out of range --
// the framer signals connection closure WITHOUT growing the buffer past the
// cap. The actual socket teardown (close + deregister readiness) lives in the
// event loop; this component only decides *that* the connection must be closed.
//
// Allocation-light and hot-path safe: the buffer is a single fixed-capacity
// array embedded in the object; consume() neither allocates nor throws nor
// reads a clock. Whole frames sitting contiguously in the supplied chunk are
// decoded in place (zero-copy); only a trailing partial frame is copied into
// the retained buffer, so buffer occupancy never exceeds one frame length.

#ifndef HME_FRAME_BUFFER_HPP
#define HME_FRAME_BUFFER_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/network.hpp"
#include "hme/wire_protocol.hpp"

namespace hme::network {

// Outcome of feeding a chunk of socket bytes to the framer.
enum class FrameStatus : std::uint8_t {
    // Bytes accepted; zero or more complete Binary_Messages were delivered and
    // any leftover partial frame is retained for the next read.
    Ok = 0,
    // An oversize or undecodable frame was detected. The buffer was not grown
    // past the kMaxMessageSize cap and the caller must tear the connection
    // down. Once closed, the framer stays closed.
    Closed = 1,
};

class FrameBuffer {
public:
    FrameBuffer() noexcept = default;

    // Feed bytes from a single (possibly partial) socket read. Every complete
    // Binary_Message that becomes available is decoded and passed to `deliver`
    // (a callable invocable with `const BinaryMessage&`) in arrival order;
    // leftover partial bytes are retained for a subsequent call.
    //
    // Returns FrameStatus::Closed if a frame's declared type is unknown, its
    // length would exceed the kMaxMessageSize cap, or its bytes fail to decode.
    // The retained buffer is never grown past the cap. Once closed, further
    // calls are no-ops that return Closed.
    template <typename Deliver>
    FrameStatus consume(std::span<const std::uint8_t> chunk,
                        Deliver&& deliver) noexcept {
        if (closed_) {
            return FrameStatus::Closed;
        }

        std::size_t off = 0;  // bytes of `chunk` already consumed

        for (;;) {
            if (len_ == 0) {
                // No partial frame retained: frame directly out of the chunk
                // (zero-copy) for as long as whole frames are available.
                const std::size_t avail = chunk.size() - off;
                if (avail == 0) {
                    return FrameStatus::Ok;
                }

                const auto frame_len = frame_length(chunk[off]);
                if (!frame_len) {
                    return close();  // unknown type or oversize
                }
                const std::size_t needed = *frame_len;

                if (avail < needed) {
                    // Only a partial frame is present: retain it and wait for
                    // the rest of the bytes.
                    std::copy_n(chunk.data() + off, avail, buf_.data());
                    len_ = avail;
                    return FrameStatus::Ok;
                }

                if (!deliver_frame(chunk.subspan(off, needed),
                                   std::forward<Deliver>(deliver))) {
                    return close();  // undecodable frame
                }
                off += needed;
                continue;
            }

            // A partial head frame is retained: complete it from the chunk.
            const auto frame_len = frame_length(buf_[0]);
            if (!frame_len) {
                return close();
            }
            const std::size_t needed = *frame_len;
            const std::size_t missing = needed - len_;
            const std::size_t avail = chunk.size() - off;
            const std::size_t take = std::min(missing, avail);

            std::copy_n(chunk.data() + off, take, buf_.data() + len_);
            len_ += take;
            off += take;

            if (len_ < needed) {
                // Still incomplete and the chunk is exhausted.
                return FrameStatus::Ok;
            }

            if (!deliver_frame(std::span<const std::uint8_t>{buf_.data(), needed},
                               std::forward<Deliver>(deliver))) {
                return close();
            }
            len_ = 0;  // frame consumed; resume the zero-copy fast path
        }
    }

    // Number of retained partial-frame bytes (0 .. kMaxMessageSize). Stays well
    // below the cap because at most one in-flight frame is ever buffered.
    std::size_t buffered() const noexcept { return len_; }

    // True once an oversize/undecodable frame has forced closure.
    bool closed() const noexcept { return closed_; }

private:
    // The fixed total frame length for a leading type byte, or nullopt when the
    // byte names no known message type or the declared length exceeds the cap
    // (the conditions that make a frame undecodable/oversize).
    static constexpr std::optional<std::size_t> frame_length(
        std::uint8_t type_byte) noexcept {
        const auto type = wire::message_type_from_byte(type_byte);
        if (!type) {
            return std::nullopt;  // unknown type discriminator
        }
        const auto len = wire::message_len(*type);
        if (!len || *len > kMaxMessageSize) {
            return std::nullopt;  // oversize frame
        }
        return len;
    }

    // Decode an exactly-sized frame and hand the message to `deliver`. Returns
    // false if the bytes do not decode (an undecodable frame, e.g. an
    // out-of-range field), which the caller turns into a closure signal.
    template <typename Deliver>
    static bool deliver_frame(std::span<const std::uint8_t> frame,
                              Deliver&& deliver) noexcept {
        auto decoded = hme::decode(frame);
        if (decoded.is_err()) {
            return false;
        }
        std::forward<Deliver>(deliver)(decoded.value());
        return true;
    }

    FrameStatus close() noexcept {
        closed_ = true;
        return FrameStatus::Closed;
    }

    std::array<std::uint8_t, kMaxMessageSize> buf_{};
    std::size_t len_ = 0;
    bool closed_ = false;
};

}  // namespace hme::network

#endif  // HME_FRAME_BUFFER_HPP
