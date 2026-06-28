// End-to-end in-process assembly of the Hyper-Match-Engine data path.
//
// Wires the already-built components into the single in-process pipeline:
//
//     client bytes
//        -> Network_Server framing (FrameBuffer / ConnectionRegistry)
//        -> decoded Binary_Messages
//        -> ingress Ring_Buffer
//        -> Matching_Engine (EngineProcessor)
//        -> egress Ring_Buffer
//        -> encoded response Binary_Messages
//        -> client bytes
//
// Why this lives on the C++ side
// ------------------------------
// Both the Network_Server and the Matching_Engine are C++ and the shared
// contract between every tier is the binary Wire_Protocol Binary_Message (the
// Binary_Codec is implemented in both Rust and C++ and the two agree
// byte-for-byte). The assembly is a `ServerPipeline` type that OWNS the
// connection registry (framing + lifecycle + the 10,000-connection cap) and the
// engine processor (ingress ring + matching core + egress ring), and routes
// framed inbound commands into the engine and drains egress events back out to
// the originating connection.
//
// Where the Rust Gateway composes
// -------------------------------
// The Rust Gateway (gateway/src/lib.rs) is the client-edge trust boundary. Its
// role is JSON<->binary translation at the very edge:
//
//   * Inbound:  JSON order -> validate / assign id / range-check ->
//               `NewOrder` (or `CancelOrder`) Binary_Message bytes.
//   * Outbound: response Binary_Message bytes -> JSON response, mapping engine
//               Reject reasons to HTTP status.
//
// Because the Gateway emits and consumes exactly the same Wire_Protocol bytes
// this C++ pipeline frames and encodes, the Gateway composes at the network
// boundary purely through the Binary_Message contract: the bytes a Gateway
// produces for an accepted order are exactly the bytes `submit_client_bytes`
// frames into a `NewOrder`, and the response bytes this pipeline emits are
// exactly the bytes `Gateway::handle_engine_response` decodes. No engine
// internals cross the boundary; only Wire_Protocol bytes do. The cross-language
// process boundary (Rust Gateway <-> C++ server) is therefore a byte pipe,
// which keeps this assembly a pure C++ composition.
//
// Routing model (in-process, single-threaded)
// --------------------------------------------
// The Matching_Engine is single-threaded and deterministic.
// `submit_client_bytes` drives one connection's read to completion
// synchronously: it frames the inbound bytes (submitting each decoded command
// to the ingress ring), drains the ingress ring through the engine, then drains
// every egress event the batch produced and hands the encoded response bytes to
// the caller's outbound sink. Because processing is synchronous and the egress
// ring is fully drained per call, the events emitted belong to the batch just
// submitted, so they route back to the originating connection without an
// order-id routing table. A concurrent multi-client deployment would add such a
// table (order_id -> connection); that is a deployment concern of the full
// event-loop server, out of scope for this data-path assembly.
//
// Hot-path discipline
// -------------------
// Every step composed here is allocation/exception/clock free on the data path:
// the framer reassembles in place, the rings are pre-allocated, the engine is
// the zero-allocation matching core, and response encoding targets a fixed
// stack buffer sized to the largest Wire_Protocol message. The only allocation
// is the per-connection state the ConnectionRegistry creates on accept and
// releases on teardown, exactly as in the standalone server.

#ifndef HME_SERVER_PIPELINE_HPP
#define HME_SERVER_PIPELINE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hme/binary_message.hpp"
#include "hme/codec.hpp"
#include "hme/connection_registry.hpp"
#include "hme/engine_processor.hpp"
#include "hme/network.hpp"
#include "hme/wire_protocol.hpp"

namespace hme::server {

// Default capacities for a ready-to-use pipeline. Callers that need different
// sizing instantiate ServerPipeline with explicit template arguments.
inline constexpr std::size_t kDefaultMaxOrders = 1 << 16;       // 65,536 resting orders
inline constexpr std::size_t kDefaultMaxPriceLevels = 1 << 14;  // 16,384 price levels
inline constexpr std::size_t kDefaultRingCapacity = 1 << 12;    // 4,096 messages

// The assembled in-process data path: Network_Server framing -> ingress ring ->
// Matching_Engine -> egress ring -> response bytes.
//
//   MaxOrders        - resting-order pool capacity (both sides combined).
//   MaxPriceLevels   - price-level pool capacity (both sides combined).
//   IngressCapacity  - fixed capacity of the inbound command ring.
//   EgressCapacity   - fixed capacity of the outbound event ring.
//
// A ReadinessPoller (the OS-native epoll/IOCP adapter, or a test fake) is
// injected so the pipeline owns connection framing and lifecycle exactly as the
// standalone Network_Server does; `poller` must outlive the pipeline.
template <std::size_t MaxOrders = kDefaultMaxOrders,
          std::size_t MaxPriceLevels = kDefaultMaxPriceLevels,
          std::size_t IngressCapacity = kDefaultRingCapacity,
          std::size_t EgressCapacity = IngressCapacity>
class ServerPipeline {
public:
    using Processor = engine::EngineProcessor<MaxOrders, MaxPriceLevels,
                                              IngressCapacity, EgressCapacity>;
    using EngineConfig = typename Processor::EngineConfig;

    // Build the pipeline over an injected readiness poller. The connection cap
    // defaults to the limit of 10,000 and is injectable so tests can exercise
    // the cap cheaply. The engine begins operational (all ring/book storage is
    // reserved inline); call initialize() to validate caller-requested
    // capacities and model the startup-reservation path.
    explicit ServerPipeline(
        network::ReadinessPoller& poller,
        std::uint32_t max_connections = network::kMaxConnections) noexcept
        : registry_(poller, max_connections) {}

    // ---- Startup -----------------------------------------------------------

    // Reserve/validate engine working memory and bring the processor to its
    // operational state. Returns false (and leaves the pipeline non-operational)
    // when the requested capacities cannot be satisfied.
    bool initialize(const EngineConfig& config = EngineConfig{}) noexcept {
        return processor_.initialize(config);
    }

    // True once the engine is operational and able to process commands.
    bool operational() const noexcept { return processor_.operational(); }

    // ---- Connection lifecycle ----------------------------------------------

    // Offer a freshly-accepted client socket to the pipeline. Registers it for
    // readiness and creates its framing state, or rejects + releases it when
    // the connection cap is reached.
    network::AcceptResult accept(network::SocketHandle fd) {
        return registry_.accept(fd);
    }

    // Tear a connection down through the single teardown path: stop monitoring,
    // close the socket, release per-connection state.
    void teardown(network::SocketHandle fd, network::TeardownReason reason) {
        registry_.teardown(fd, reason);
    }

    // ---- The assembled data path -------------------------------------------

    // Drive one socket read for connection `fd` through the entire pipeline.
    //
    //   1. Frame `bytes` into complete Binary_Messages: the Network_Server
    //      framing logic reassembles whole frames and buffers any partial
    //      trailing frame for the next read. Each complete command is submitted
    //      to the engine ingress ring.
    //   2. Drain the ingress ring through the Matching_Engine, which matches /
    //      cancels and emits events to the egress ring.
    //   3. Drain the egress ring, encode each event back into Wire_Protocol
    //      bytes, and hand them to `out` for return to the client.
    //
    // `out` is any callable invocable as
    // `out(network::SocketHandle, std::span<const std::uint8_t>)`; it receives
    // the originating socket and the encoded response frame for each emitted
    // event, in emission order.
    //
    // Returns the framing status: FrameStatus::Ok when the connection remains
    // open, or FrameStatus::Closed when an oversize/undecodable frame forced the
    // connection's teardown. Even on closure, commands framed before the bad
    // frame are still processed and their responses delivered to `out`.
    //
    // No-op returning FrameStatus::Closed when the engine is not operational
    // (an engine whose startup reservation failed processes no Order).
    template <typename OutSink>
    network::FrameStatus submit_client_bytes(network::SocketHandle fd,
                                             std::span<const std::uint8_t> bytes,
                                             OutSink&& out) {
        if (!processor_.operational()) {
            return network::FrameStatus::Closed;
        }

        // Step 1: frame inbound bytes and submit each decoded command to the
        // ingress ring. An ingress-full push is back-pressure and is counted
        // rather than silently lost.
        const bool open = registry_.on_readable(
            fd, bytes, [this](const BinaryMessage& msg) {
                if (!processor_.submit(msg)) {
                    ++ingress_backpressure_count_;
                }
            });

        // Step 2: run the engine over everything just enqueued (FIFO).
        processor_.process_all();

        // Step 3: drain egress events back to the originating connection.
        drain_events(fd, out);

        return open ? network::FrameStatus::Ok : network::FrameStatus::Closed;
    }

    // ---- Observers ----------------------------------------------------------

    const Processor& processor() const noexcept { return processor_; }
    Processor& processor() noexcept { return processor_; }

    const network::ConnectionRegistry& registry() const noexcept {
        return registry_;
    }
    network::ConnectionRegistry& registry() noexcept { return registry_; }

    // Number of inbound commands refused because the ingress ring was full
    // (back-pressure). Stays 0 when the ring is sized for the offered load.
    std::uint64_t ingress_backpressure_count() const noexcept {
        return ingress_backpressure_count_;
    }

    // Number of response events emitted to outbound sinks over the pipeline's
    // lifetime.
    std::uint64_t responses_emitted() const noexcept {
        return responses_emitted_;
    }

private:
    // Drain every queued egress event, encode it, and deliver the bytes to the
    // outbound sink for connection `fd`, in emission (FIFO) order.
    template <typename OutSink>
    void drain_events(network::SocketHandle fd, OutSink&& out) {
        BinaryMessage event;
        while (processor_.next_event(event)) {
            std::array<std::uint8_t, wire::kMaxMessageLen> frame{};
            const auto encoded =
                encode(event, std::span<std::uint8_t>{frame.data(), frame.size()});
            // A well-formed engine event always encodes; guard defensively so a
            // hypothetical encode failure cannot emit a malformed frame.
            if (encoded.is_ok()) {
                out(fd, std::span<const std::uint8_t>{frame.data(), encoded.value()});
                ++responses_emitted_;
            }
        }
    }

    Processor processor_{};                 // ingress ring + engine + egress ring.
    network::ConnectionRegistry registry_;  // framing + lifecycle + cap.
    std::uint64_t ingress_backpressure_count_ = 0;
    std::uint64_t responses_emitted_ = 0;
};

}  // namespace hme::server

#endif  // HME_SERVER_PIPELINE_HPP
