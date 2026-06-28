// Oversize-frame rejection. For any byte stream in which a single
// Binary_Message would exceed 65,536 bytes (kMaxMessageSize) -- realized here by
// an undecodable leading type byte that names no known message type and
// therefore has no bounded frame length -- the Network_Server's framing logic
// signals the connection for closure rather than delivering a message or
// growing its buffer past the cap.
//
// In this fixed-layout Wire_Protocol every known message type has a small fixed
// length (the largest, Trade, is 37 bytes), so no *valid* frame can ever exceed
// the 65,536-byte cap. The case where the bytes received for a single
// Binary_Message exceed the maximum size is exactly the case where the framer
// cannot bound a frame: an unknown leading type byte (>= 5) names no type, so
// its length is unbounded and the framer must refuse to keep accumulating.
// FrameBuffer maps that to FrameStatus::Closed without growing its buffer past
// kMaxMessageSize.
//
// A generator produces a byte stream of zero-or-more leading *valid* frames
// (which must be delivered in order first) followed by an unknown type byte
// (>= 5) at an arbitrary frame boundary plus arbitrary trailing garbage,
// optionally split into arbitrary read chunks. The property asserts that:
//   - consume() eventually returns FrameStatus::Closed and closed() is true,
//   - buffered() never exceeds kMaxMessageSize at any point,
//   - exactly the leading valid frames are delivered (in order) and no
//     spurious message is delivered at or past the bad frame.
//
// All RapidCheck generators below are defined locally in an anonymous namespace
// (internal linkage) with distinct `_p23` names. This deliberately avoids ODR /
// duplicate-symbol clashes with rc::Arbitrary<...> specializations or helper
// names defined in the other property-test translation units that link into the
// same hme_tests executable.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/frame_buffer.hpp"
#include "hme/network.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;
using hme::network::FrameBuffer;
using hme::network::FrameStatus;
using hme::network::kMaxMessageSize;

namespace {

// ---------------------------------------------------------------------------
// One generated oversize-rejection case: a byte stream consisting of `valid`
// fully-encoded leading frames (which the framer must deliver in order),
// followed by an unknown type byte + trailing garbage that must force closure,
// plus the chunk sizes used to feed the stream to the framer.
// ---------------------------------------------------------------------------
struct OversizeCase_p23 {
    std::vector<BinaryMessage> valid;     // leading frames, must be delivered
    std::vector<std::uint8_t> stream;     // valid frames + bad byte + garbage
    std::vector<std::size_t> chunk_lens;  // arbitrary read-chunk sizes (>= 1)
};

// Append the wire-encoding of `msg` to `out`.
void append_encoded_p23(std::vector<std::uint8_t>& out, const BinaryMessage& m) {
    const std::size_t at = out.size();
    out.resize(at + encoded_len_of(m));
    const auto res =
        encode(m, std::span<std::uint8_t>{out.data() + at, encoded_len_of(m)});
    RC_ASSERT(res.is_ok());
}

// In-range price_ticks: [kMinPriceTicks, kMaxPriceTicks].
rc::Gen<std::uint64_t> gen_price_p23() {
    return rc::gen::inRange<std::uint64_t>(limits::kMinPriceTicks,
                                           limits::kMaxPriceTicks + 1);
}

// In-range quantity: [kMinGatewayQuantity, kMaxGatewayQuantity].
rc::Gen<std::uint32_t> gen_qty_p23() {
    return rc::gen::inRange<std::uint32_t>(limits::kMinGatewayQuantity,
                                           limits::kMaxGatewayQuantity + 1);
}

// A single valid, in-range Binary_Message of an arbitrary type. These are the
// frames that must be delivered intact before the bad frame is reached.
rc::Gen<BinaryMessage> gen_valid_message_p23() {
    auto new_order = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::element(Side::Buy, Side::Sell), gen_price_p23(),
                       gen_qty_p23(), rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, Side, std::uint64_t, std::uint32_t,
                            std::uint64_t>& t) {
            return BinaryMessage{NewOrder{std::get<0>(t), std::get<1>(t),
                                          std::get<2>(t), std::get<3>(t),
                                          std::get<4>(t)}};
        });

    auto cancel = rc::gen::map(
        rc::gen::arbitrary<std::uint64_t>(),
        [](std::uint64_t id) { return BinaryMessage{CancelOrder{id}}; });

    auto trade = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_price_p23(),
                       gen_qty_p23(), rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                            std::uint64_t, std::uint64_t>& t) {
            return BinaryMessage{Trade{std::get<0>(t), std::get<1>(t),
                                       std::get<2>(t), std::get<3>(t),
                                       std::get<4>(t)}};
        });

    auto ack = rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::element(AckKind::Accepted, AckKind::Cancelled)),
        [](const std::tuple<std::uint64_t, AckKind>& t) {
            return BinaryMessage{Ack{std::get<0>(t), std::get<1>(t)}};
        });

    auto reject = rc::gen::map(
        rc::gen::tuple(
            rc::gen::arbitrary<std::uint64_t>(),
            rc::gen::element(RejectReason::InvalidPrice,
                             RejectReason::InvalidQuantity,
                             RejectReason::OrderNotFound,
                             RejectReason::NoLongerResting,
                             RejectReason::IntegrityViolation)),
        [](const std::tuple<std::uint64_t, RejectReason>& t) {
            return BinaryMessage{Reject{std::get<0>(t), std::get<1>(t)}};
        });

    return rc::gen::oneOf(std::move(new_order), std::move(cancel),
                          std::move(trade), std::move(ack), std::move(reject));
}

// A full oversize-rejection case.
rc::Gen<OversizeCase_p23> gen_oversize_case_p23() {
    return rc::gen::mapcat(
        // 0..6 leading valid frames that must be delivered first.
        rc::gen::inRange<std::size_t>(0, 7),
        [](std::size_t valid_count) {
            return rc::gen::map(
                rc::gen::tuple(
                    rc::gen::container<std::vector<BinaryMessage>>(
                        valid_count, gen_valid_message_p23()),
                    // The unknown leading type byte that begins the bad frame.
                    rc::gen::inRange<int>(5, 256),
                    // Arbitrary trailing garbage after the bad byte.
                    rc::gen::arbitrary<std::vector<std::uint8_t>>(),
                    // Chunk sizes (each >= 1) used to walk the stream; an empty
                    // list means "feed the whole stream in one read".
                    rc::gen::container<std::vector<std::size_t>>(
                        rc::gen::inRange<std::size_t>(1, kMaxMessageSize + 1))),
                [](const std::tuple<std::vector<BinaryMessage>, int,
                                    std::vector<std::uint8_t>,
                                    std::vector<std::size_t>>& t) {
                    OversizeCase_p23 c;
                    c.valid = std::get<0>(t);

                    for (const auto& m : c.valid) {
                        append_encoded_p23(c.stream, m);
                    }
                    // Bad frame: an unknown type discriminator at a frame
                    // boundary, followed by arbitrary garbage.
                    c.stream.push_back(
                        static_cast<std::uint8_t>(std::get<1>(t)));
                    const auto& tail = std::get<2>(t);
                    c.stream.insert(c.stream.end(), tail.begin(), tail.end());

                    c.chunk_lens = std::get<3>(t);
                    return c;
                });
        });
}

// A deliver sink that records every delivered Binary_Message in order.
struct Sink_p23 {
    std::vector<BinaryMessage> messages;
    void operator()(const BinaryMessage& m) { messages.push_back(m); }
};

}  // namespace

TEST_CASE(
    "Property 23: an undecodable/oversize frame forces closure without "
    "delivering a message or growing the buffer past the cap",
    "[frame][network][property][oversize]") {
    const bool ok = rc::check(
        "unknown-type frame -> Closed, leading valid frames delivered, "
        "buffer bounded",
        [] {
            const OversizeCase_p23 c = *gen_oversize_case_p23();

            FrameBuffer fb;
            Sink_p23 sink;
            bool saw_closed = false;

            std::size_t off = 0;
            std::size_t chunk_idx = 0;
            while (off < c.stream.size()) {
                std::size_t take = c.stream.size() - off;
                if (chunk_idx < c.chunk_lens.size()) {
                    take = std::min(take, c.chunk_lens[chunk_idx]);
                }
                ++chunk_idx;

                const auto status = fb.consume(
                    std::span<const std::uint8_t>{c.stream.data() + off, take},
                    sink);
                off += take;

                // The buffer must never grow past the cap.
                RC_ASSERT(fb.buffered() <= kMaxMessageSize);

                if (status == FrameStatus::Closed) {
                    saw_closed = true;
                    RC_ASSERT(fb.closed());
                    break;  // once closed, further reads are no-op Closed
                }
            }

            // The stream always contains an unknown leading type byte at a
            // frame boundary, so closure must have been signalled.
            RC_ASSERT(saw_closed);
            RC_ASSERT(fb.closed());

            // Exactly the leading valid frames are delivered, in order; no
            // spurious message is delivered at or past the bad frame.
            RC_ASSERT(sink.messages.size() == c.valid.size());
            for (std::size_t i = 0; i < c.valid.size(); ++i) {
                RC_ASSERT(sink.messages[i] == c.valid[i]);
            }

            // After closure, any further read is a no-op that stays Closed and
            // delivers nothing more.
            const std::size_t before = sink.messages.size();
            const std::array<std::uint8_t, 1> more{0};
            RC_ASSERT(fb.consume(std::span<const std::uint8_t>{more}, sink) ==
                      FrameStatus::Closed);
            RC_ASSERT(sink.messages.size() == before);
            RC_ASSERT(fb.buffered() <= kMaxMessageSize);
        });
    CHECK(ok);
}
