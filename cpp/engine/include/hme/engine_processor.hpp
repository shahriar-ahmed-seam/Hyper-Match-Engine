// Matching_Engine FIFO processing loop.
//
// The single-threaded driver that ties the pre-allocated ingress/egress
// Ring_Buffers to the matching/cancellation core. It reads decoded
// Binary_Messages from an ingress ring and writes outbound event
// Binary_Messages to an egress ring, with no I/O, no clock reads, no
// exceptions, and no dynamic allocation, so the whole loop is hot-path safe.
//
// The engine only sees decoded in-memory message structs read from the ingress
// ring (the Gateway owns encode/decode), so the ingress ring carries decoded
// `BinaryMessage` values. The only two commands an engine accepts are NewOrder
// and CancelOrder; the remaining variants (Trade, Ack, Reject) are outbound
// event types and are never valid inbound commands.
//
// process_next():
//
//   * Dequeues exactly ONE message from the ingress ring in strict first-in-
//     first-out order: try_pop returns the oldest enqueued message and the loop
//     never reorders, skips, or processes concurrently. All processing happens
//     inline on the calling thread.
//
//   * Dispatches by message type:
//       - NewOrder    -> validate then match in price-time priority, emitting
//                        Trade events (and resting any remainder) or, on
//                        validation failure, exactly one Reject error event.
//       - CancelOrder -> remove the resting order and Ack, or emit exactly one
//                        cancellation Reject when it is not resting.
//
//   * Continue-on-error: a message that fails validation, or an out-of-place
//     outbound-event variant placed on the ingress ring, causes the engine to
//     emit exactly ONE error event (a Reject) for that message and then
//     continue with the next dequeued message. A rejected message never
//     disturbs the deterministic processing of the messages that follow it.
//
// Every emitted event is pushed to the egress ring as a Binary_Message; the
// consumer (Gateway) drains it FIFO. If the egress ring is momentarily full the
// push is refused (back-pressure) and counted rather than allocating; callers
// size the egress ring to hold the events a batch of ingress messages can
// produce.
//
// initialize(config) models the startup memory-reservation / abort-on-failure
// path. Because every ring and book pool is held inline and reserved at
// construction (the compile-time fixed-capacity design), initialize() validates
// that the capacities a caller requires can be satisfied by the reserved fixed
// capacities and, on success, brings the processor to its operational state
// with a clean, empty book. If the required Ring_Buffer or Order_Book working
// memory cannot be reserved - because a requested capacity exceeds the reserved
// fixed capacity, or because a reservation failure is injected for testing -
// initialize() aborts: it records an InsufficientMemory startup error, leaves
// the processor outside the operational state, and returns false. While the
// processor is not operational, process_next() refuses to run, so no Order is
// ever processed by an engine whose startup reservation failed.

#ifndef HME_ENGINE_PROCESSOR_HPP
#define HME_ENGINE_PROCESSOR_HPP

#include <cstddef>
#include <cstdint>
#include <variant>

#include "hme/binary_message.hpp"
#include "hme/matching_engine.hpp"
#include "hme/ring_buffer.hpp"
#include "hme/wire_protocol.hpp"

namespace hme::engine {

// Outcome of the startup memory-reservation step. A processor that has not
// aborted startup reports StartupError::None; a processor whose required
// Ring_Buffer or Order_Book working memory could not be reserved reports
// StartupError::InsufficientMemory and remains outside the operational state.
enum class StartupError : std::uint8_t {
    None = 0,                // startup succeeded (or not yet attempted to fail).
    InsufficientMemory = 1,  // required working memory could not be reserved.
};

// Single-threaded FIFO processing loop over a pair of pre-allocated rings.
//
//   MaxOrders        - resting-order pool capacity (both sides combined).
//   MaxPriceLevels   - price-level pool capacity (both sides combined).
//   IngressCapacity  - fixed capacity of the inbound command ring.
//   EgressCapacity   - fixed capacity of the outbound event ring (defaults to
//                      the ingress capacity).
//
// All storage (book pools and both rings) is held inline and reserved when the
// processor is constructed; no operation grows it.
template <std::size_t MaxOrders, std::size_t MaxPriceLevels,
          std::size_t IngressCapacity, std::size_t EgressCapacity = IngressCapacity>
class EngineProcessor {
public:
    using Engine = MatchingEngine<MaxOrders, MaxPriceLevels>;
    using IngressRing = RingBuffer<BinaryMessage, IngressCapacity>;
    using EgressRing = RingBuffer<BinaryMessage, EgressCapacity>;

    EngineProcessor() noexcept = default;

    // ---- Startup memory reservation ----------------------------------------

    // The working-memory capacities a caller requires at startup. With the
    // compile-time inline-storage design every capacity is reserved when the
    // processor is constructed; this config lets a caller declare the capacities
    // it actually intends to use so initialize() can confirm the reserved fixed
    // capacities are large enough to satisfy them. Each field defaults to the
    // corresponding reserved fixed capacity, so a default-constructed config is
    // always satisfiable.
    struct EngineConfig {
        std::size_t max_orders = MaxOrders;
        std::size_t max_price_levels = MaxPriceLevels;
        std::size_t ingress_capacity = IngressCapacity;
        std::size_t egress_capacity = EgressCapacity;

        // Test-only injection hook. When true, initialize() simulates a
        // working-memory reservation failure regardless of the requested
        // capacities, exercising the abort-on-failure path without needing an
        // actual out-of-memory condition.
        bool force_reservation_failure = false;
    };

    // Reserve all Ring_Buffer and Order_Book working memory at the configured
    // fixed capacities and bring the processor to its operational state. Returns
    // true on success.
    //
    // If the required working memory cannot be reserved - a requested capacity
    // exceeds the reserved fixed capacity, or a reservation failure is injected
    // via EngineConfig::force_reservation_failure - initialize() aborts startup:
    // it records StartupError::InsufficientMemory, leaves the processor OUTSIDE
    // the operational state (operational() stays false), and returns false. A
    // processor that is not operational refuses to process any message (see
    // process_next()), so no Order is ever processed by an engine whose startup
    // reservation failed.
    //
    // On success the book is reset to a clean empty state, the startup error is
    // cleared, and the processor becomes operational. Never allocates (the
    // storage is already reserved inline), throws, or reads a clock.
    bool initialize(const EngineConfig& config = EngineConfig{}) noexcept {
        // Leave/return the processor to a non-operational state until the
        // reservation is confirmed, so a failed initialize() can never leave a
        // stale operational flag set.
        operational_ = false;

        const bool reservation_failed =
            config.force_reservation_failure ||
            config.max_orders > MaxOrders ||
            config.max_price_levels > MaxPriceLevels ||
            config.ingress_capacity > IngressCapacity ||
            config.egress_capacity > EgressCapacity;

        if (reservation_failed) {
            // Abort startup: insufficient working memory, remain non-operational
            // so that no Order is processed.
            startup_error_ = StartupError::InsufficientMemory;
            return false;
        }

        // Reservation satisfied. The inline ring/book storage is already
        // present; reset the book to a known-empty state and enter operation.
        engine_.book().clear();
        engine_.clear_retired();
        startup_error_ = StartupError::None;
        operational_ = true;
        return true;
    }

    // True once initialize() has successfully reserved all working memory; false
    // before initialization or after a reservation failure. process_next()
    // refuses to run while this is false.
    //
    // Note: with the compile-time inline-storage design a freshly constructed
    // processor already has all of its memory reserved, so it begins in the
    // operational state; initialize() exists to validate caller-requested
    // capacities and to model the abort-on-failure path.
    bool operational() const noexcept { return operational_; }

    // The reason startup aborted, or StartupError::None when no abort occurred.
    StartupError startup_error() const noexcept { return startup_error_; }

    // ---- Ingress (producer side) -------------------------------------------

    // Enqueue an inbound message onto the ingress ring in FIFO order. Returns
    // true on success, or false (a back-pressure indication) when the ingress
    // ring is at capacity, in which case every buffered message is left
    // unchanged and no allocation occurs. The producer (the Gateway) calls
    // this; processing happens later in process_next().
    bool submit(const BinaryMessage& msg) noexcept {
        return ingress_.try_push(msg);
    }
    bool submit(BinaryMessage&& msg) noexcept {
        return ingress_.try_push(std::move(msg));
    }

    // Convenience overloads for the two valid inbound command types.
    bool submit(const NewOrder& order) noexcept {
        return ingress_.try_push(BinaryMessage{order});
    }
    bool submit(const CancelOrder& cancel) noexcept {
        return ingress_.try_push(BinaryMessage{cancel});
    }

    // ---- Processing loop ----------------------------------------------------

    // Dequeue exactly ONE message from the ingress ring (FIFO), dispatch it,
    // and emit any resulting events to the egress ring. Returns true if a
    // message was processed, or false if the ingress ring was empty (nothing to
    // do) OR the processor is not operational (an engine whose startup
    // reservation failed processes no Order). Single message per call so callers
    // retain full control over scheduling; no reordering or skipping. Never
    // allocates, throws, or reads a clock.
    bool process_next() {
        if (!operational_) {
            return false;  // non-operational -> never process.
        }
        BinaryMessage msg;
        if (!ingress_.try_pop(msg)) {
            return false;  // ingress empty.
        }
        ++processed_count_;
        dispatch(msg);
        return true;
    }

    // Process every message currently queued on the ingress ring, in FIFO
    // order, until it is empty. Returns the number of messages processed. (Only
    // messages present when the call begins-and any the loop itself does not
    // enqueue-are drained; the engine never produces ingress messages.)
    std::size_t process_all() {
        std::size_t count = 0;
        while (process_next()) {
            ++count;
        }
        return count;
    }

    // ---- Egress (consumer side) --------------------------------------------

    // Pop the oldest outbound event from the egress ring in FIFO order. Returns
    // true and writes it to `out`, or false (leaving `out` unchanged) when the
    // egress ring is empty.
    bool next_event(BinaryMessage& out) noexcept { return egress_.try_pop(out); }

    // ---- Observers ----------------------------------------------------------

    const Engine& engine() const noexcept { return engine_; }
    Engine& engine() noexcept { return engine_; }

    const IngressRing& ingress() const noexcept { return ingress_; }
    const EgressRing& egress() const noexcept { return egress_; }

    // Total number of ingress messages dequeued and dispatched over the
    // processor's lifetime.
    std::uint64_t processed_count() const noexcept { return processed_count_; }

    // Number of error events emitted for *invalid* dequeued messages: NewOrder
    // values that failed validation and out-of-place outbound variants placed
    // on the ingress ring. Exactly one error event is counted per invalid
    // message. Cancellation rejections for a not-resting order are a normal
    // outcome of a well-formed CancelOrder and are NOT counted here.
    std::uint64_t error_event_count() const noexcept { return error_event_count_; }

    // Number of events that could not be pushed because the egress ring was
    // full. Remains 0 when the egress ring is sized to hold the events a batch
    // of ingress messages produces. Exposed so callers can detect drops without
    // inspecting internals.
    std::uint64_t egress_overflow_count() const noexcept {
        return egress_overflow_count_;
    }

private:
    // Reject reason used for an out-of-place outbound variant (Trade, Ack, or
    // Reject) found on the ingress ring. The Wire_Protocol defines no dedicated
    // "malformed/unknown command" reason, so such protocol errors are reported
    // with the engine's generic refusal reason; the distinct, observable
    // error_event_count() lets callers count them unambiguously regardless of
    // the reason byte.
    static constexpr RejectReason kInvalidMessageReason =
        RejectReason::IntegrityViolation;

    // Route one decoded inbound message to the matching core or, when it is not
    // a valid command, to the invalid-message error path.
    void dispatch(const BinaryMessage& msg) {
        if (const NewOrder* order = std::get_if<NewOrder>(&msg)) {
            // Validate then match. On a validation failure the two-sink overload
            // emits exactly one Reject via emit_reject and leaves the book
            // unchanged; that Reject is this message's single error event.
            const bool accepted = engine_.process_new_order(
                *order, [this](const Trade& t) { emit(BinaryMessage{t}); },
                [this](const Reject& r) {
                    emit(BinaryMessage{r});
                    ++error_event_count_;
                });
            // An accepted NewOrder emits its 0+ Trade events followed by exactly
            // one terminating Ack(Accepted) carrying the order id. A NewOrder
            // that failed validation was already rejected above and produces no
            // Accepted ack. The Accepted ack is emitted here in the processing
            // loop (not in the matching core), so direct MatchingEngine users
            // are unaffected.
            if (accepted) {
                Ack ack;
                ack.order_id = order->order_id;
                ack.kind = AckKind::Accepted;
                emit(BinaryMessage{ack});
            }
            return;
        }
        if (const CancelOrder* cancel = std::get_if<CancelOrder>(&msg)) {
            // Remove + Ack or emit exactly one cancellation Reject when not
            // resting. A not-resting rejection is a normal outcome of a valid
            // command, so it is emitted but not counted as an invalid-message
            // error.
            engine_.process_cancel_order(
                *cancel, [this](const Ack& a) { emit(BinaryMessage{a}); },
                [this](const Reject& r) { emit(BinaryMessage{r}); });
            return;
        }
        // Any other variant is an outbound event type that does not belong on
        // the ingress ring: an invalid inbound message. Emit exactly one error
        // event and continue.
        emit_invalid_message_error(msg);
    }

    // Emit the single Reject error event for an out-of-place inbound variant,
    // carrying whatever order identifier the variant exposes (0 when none).
    void emit_invalid_message_error(const BinaryMessage& msg) {
        Reject reject;
        reject.order_id = invalid_message_order_id(msg);
        reject.reason = kInvalidMessageReason;
        emit(BinaryMessage{reject});
        ++error_event_count_;
    }

    // Best-effort order identifier for an out-of-place inbound variant, used
    // only to populate the error Reject. Trade carries no single order id, so
    // its incoming_id is reported; Ack/Reject carry order_id directly.
    static std::uint64_t invalid_message_order_id(
        const BinaryMessage& msg) noexcept {
        if (const Trade* t = std::get_if<Trade>(&msg)) {
            return t->incoming_id;
        }
        if (const Ack* a = std::get_if<Ack>(&msg)) {
            return a->order_id;
        }
        if (const Reject* r = std::get_if<Reject>(&msg)) {
            return r->order_id;
        }
        return 0;
    }

    // Push one outbound event onto the egress ring. A full egress ring refuses
    // the push (back-pressure) and is counted rather than allocating.
    void emit(const BinaryMessage& event) noexcept {
        if (!egress_.try_push(event)) {
            ++egress_overflow_count_;
        }
    }

    Engine engine_{};            // matching/cancellation core + book storage.
    IngressRing ingress_{};      // inbound decoded commands (FIFO).
    EgressRing egress_{};        // outbound events (FIFO).
    std::uint64_t processed_count_ = 0;
    std::uint64_t error_event_count_ = 0;
    std::uint64_t egress_overflow_count_ = 0;

    // Startup state. A freshly constructed processor already has all of its
    // storage reserved inline, so it begins operational; initialize() may move
    // it out of the operational state on a reservation failure and records why
    // in startup_error_.
    bool operational_ = true;
    StartupError startup_error_ = StartupError::None;
};

}  // namespace hme::engine

#endif  // HME_ENGINE_PROCESSOR_HPP
