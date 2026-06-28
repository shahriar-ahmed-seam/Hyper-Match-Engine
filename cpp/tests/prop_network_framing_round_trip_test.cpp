// Network framing round trip. For any sequence of complete Binary_Messages
// concatenated into a byte stream and split into arbitrary read chunks
// (including chunks that bisect a message), the Network_Server's framing logic
// delivers exactly the original sequence of complete Binary_Messages, buffering
// partial frames up to 65,536 bytes.
//
// The generators are defined locally in an anonymous namespace with a `_p22`
// suffix (rather than as `rc::Arbitrary<...>` specializations) so they cannot
// clash, at link time, with the codec property tests that specialize
// Arbitrary<BinaryMessage> for the same executable.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/frame_buffer.hpp"
#include "hme/wire_protocol.hpp"

using namespace hme;
using hme::network::FrameBuffer;
using hme::network::FrameStatus;

namespace {

// --- Local generators (suffix _p22) ---------------------------------------
//
// Each produces a *valid* Binary_Message: enum fields are drawn from their full
// discriminator set and the range-checked numeric fields (price_ticks,
// quantity) are constrained to the Wire_Protocol permitted ranges so encode()
// always succeeds. Other integer fields span the full domain to exercise
// boundary byte patterns.

rc::Gen<Side> gen_side_p22() { return rc::gen::element(Side::Buy, Side::Sell); }

rc::Gen<AckKind> gen_ack_kind_p22() {
    return rc::gen::element(AckKind::Accepted, AckKind::Cancelled);
}

rc::Gen<RejectReason> gen_reject_reason_p22() {
    return rc::gen::element(RejectReason::InvalidPrice,
                            RejectReason::InvalidQuantity,
                            RejectReason::OrderNotFound,
                            RejectReason::NoLongerResting,
                            RejectReason::IntegrityViolation);
}

rc::Gen<std::uint64_t> gen_price_ticks_p22() {
    // gen::inRange is half-open, so the upper bound is +1.
    return rc::gen::inRange<std::uint64_t>(limits::kMinPriceTicks,
                                           limits::kMaxPriceTicks + 1);
}

rc::Gen<std::uint32_t> gen_quantity_p22() {
    return rc::gen::inRange<std::uint32_t>(limits::kMinGatewayQuantity,
                                           limits::kMaxGatewayQuantity + 1);
}

rc::Gen<BinaryMessage> gen_new_order_p22() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_side_p22(),
                       gen_price_ticks_p22(), gen_quantity_p22(),
                       rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, Side, std::uint64_t, std::uint32_t,
                            std::uint64_t>& t) -> BinaryMessage {
            return NewOrder{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                            std::get<3>(t), std::get<4>(t)};
        });
}

rc::Gen<BinaryMessage> gen_cancel_order_p22() {
    return rc::gen::map(rc::gen::arbitrary<std::uint64_t>(),
                        [](std::uint64_t order_id) -> BinaryMessage {
                            return CancelOrder{order_id};
                        });
}

rc::Gen<BinaryMessage> gen_trade_p22() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_price_ticks_p22(),
                       gen_quantity_p22(), rc::gen::arbitrary<std::uint64_t>(),
                       rc::gen::arbitrary<std::uint64_t>()),
        [](const std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                            std::uint64_t, std::uint64_t>& t) -> BinaryMessage {
            return Trade{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                         std::get<3>(t), std::get<4>(t)};
        });
}

rc::Gen<BinaryMessage> gen_ack_p22() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(), gen_ack_kind_p22()),
        [](const std::tuple<std::uint64_t, AckKind>& t) -> BinaryMessage {
            return Ack{std::get<0>(t), std::get<1>(t)};
        });
}

rc::Gen<BinaryMessage> gen_reject_p22() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::arbitrary<std::uint64_t>(),
                       gen_reject_reason_p22()),
        [](const std::tuple<std::uint64_t, RejectReason>& t) -> BinaryMessage {
            return Reject{std::get<0>(t), std::get<1>(t)};
        });
}

// A valid Binary_Message of any of the five variants, uniformly chosen.
rc::Gen<BinaryMessage> gen_message_p22() {
    return rc::gen::oneOf(gen_new_order_p22(), gen_cancel_order_p22(),
                          gen_trade_p22(), gen_ack_p22(), gen_reject_p22());
}

// A (possibly empty) sequence of valid Binary_Messages.
rc::Gen<std::vector<BinaryMessage>> gen_messages_p22() {
    return rc::gen::container<std::vector<BinaryMessage>>(gen_message_p22());
}

// Encode every message back-to-back into one contiguous byte stream.
std::vector<std::uint8_t> encode_stream_p22(
    const std::vector<BinaryMessage>& msgs) {
    std::vector<std::uint8_t> stream;
    for (const auto& m : msgs) {
        const std::size_t at = stream.size();
        const std::size_t n = encoded_len_of(m);
        stream.resize(at + n);
        auto res = encode(m, std::span<std::uint8_t>{stream.data() + at, n});
        RC_ASSERT(res.is_ok());
    }
    return stream;
}

}  // namespace

TEST_CASE(
    "Property 22: framing round trip delivers the original message sequence "
    "across arbitrary chunk splits",
    "[network][frame][property][roundtrip]") {
    const bool ok = rc::check(
        "concatenate + arbitrarily re-chunk + frame == original sequence",
        []() {
            const auto msgs = *gen_messages_p22();
            const auto stream = encode_stream_p22(msgs);

            // Feed the byte stream to the framer in arbitrary chunks. Each
            // chunk size is freshly drawn in [1, remaining], so chunks include
            // single bytes and spans that bisect message boundaries (the
            // partial-frame buffering case).
            std::vector<BinaryMessage> delivered;
            auto sink = [&delivered](const BinaryMessage& m) {
                delivered.push_back(m);
            };

            FrameBuffer fb;
            std::size_t off = 0;
            while (off < stream.size()) {
                const std::size_t remaining = stream.size() - off;
                const std::size_t chunk =
                    *rc::gen::inRange<std::size_t>(1, remaining + 1);
                const auto status = fb.consume(
                    std::span<const std::uint8_t>{stream.data() + off, chunk},
                    sink);
                // A stream of well-formed frames never triggers closure.
                RC_ASSERT(status == FrameStatus::Ok);
                // Only ever one in-flight frame is buffered.
                RC_ASSERT(fb.buffered() <= wire::kMaxMessageLen);
                off += chunk;
            }

            // Every complete frame was delivered, in order, with nothing left
            // buffered.
            RC_ASSERT(fb.buffered() == 0);
            RC_ASSERT(!fb.closed());
            RC_ASSERT(delivered == msgs);
        });
    CHECK(ok);
}
