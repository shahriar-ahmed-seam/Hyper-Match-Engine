// Fixed-capacity, pre-allocated FIFO Ring_Buffer.
//
// The Matching_Engine passes orders and events through a pair of Ring_Buffers
// (ingress / egress) that are sized once at startup and never grow during
// operation. This type provides that storage:
//
//   * All slot storage is held inline (std::array member), so a RingBuffer
//     reserves its full fixed capacity when it is constructed and performs
//     zero dynamic memory allocation thereafter.
//   * try_push / try_pop are strict FIFO: elements are dequeued in the exact
//     order they were enqueued.
//   * When the buffer is full, try_push rejects the element, returns false as
//     a back-pressure indication, and leaves every buffered element unchanged,
//     without allocating.
//
// head/tail indices index a pre-allocated slot array. An explicit element count
// distinguishes the full state from the empty state while still using all
// `Capacity` slots (a count avoids the classic "sacrifice one slot" trick).

#ifndef HME_RING_BUFFER_HPP
#define HME_RING_BUFFER_HPP

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace hme::engine {

// A fixed-capacity circular FIFO queue. `Capacity` is fixed at compile time and
// all storage is contained inline within the object; constructing a RingBuffer
// reserves all of its working memory up front and no operation on it allocates.
template <typename T, std::size_t Capacity>
class RingBuffer {
    static_assert(Capacity > 0, "RingBuffer capacity must be greater than zero");

public:
    using value_type = T;
    using size_type = std::size_t;

    // The fixed number of elements this buffer can hold.
    static constexpr size_type capacity() noexcept { return Capacity; }

    // Number of elements currently buffered.
    size_type size() const noexcept { return count_; }

    bool empty() const noexcept { return count_ == 0; }
    bool full() const noexcept { return count_ == Capacity; }

    // FIFO enqueue. Copies `value` into the next free slot and returns true.
    //
    // If the buffer is already at full capacity the element is rejected: the
    // function returns false (a back-pressure indication), every previously
    // buffered element is left unchanged, and no dynamic memory is allocated.
    bool try_push(const T& value) {
        if (count_ == Capacity) {
            return false;  // full -> back-pressure, contents preserved.
        }
        slots_[tail_] = value;
        tail_ = advance(tail_);
        ++count_;
        return true;
    }

    // FIFO enqueue with move semantics (same back-pressure contract as above).
    bool try_push(T&& value) {
        if (count_ == Capacity) {
            return false;  // full -> back-pressure, contents preserved.
        }
        slots_[tail_] = std::move(value);
        tail_ = advance(tail_);
        ++count_;
        return true;
    }

    // FIFO dequeue. Moves the oldest buffered element into `out` and returns
    // true. Returns false and leaves `out` unchanged when the buffer is empty.
    // Performs no dynamic memory allocation.
    bool try_pop(T& out) {
        if (count_ == 0) {
            return false;
        }
        out = std::move(slots_[head_]);
        head_ = advance(head_);
        --count_;
        return true;
    }

private:
    // Next index in the circular array (wraps at Capacity).
    static constexpr size_type advance(size_type index) noexcept {
        return (index + 1 == Capacity) ? size_type{0} : index + 1;
    }

    std::array<T, Capacity> slots_{};  // pre-allocated, inline storage.
    size_type head_ = 0;               // index of the oldest buffered element.
    size_type tail_ = 0;               // index of the next free slot.
    size_type count_ = 0;              // number of buffered elements.
};

}  // namespace hme::engine

#endif  // HME_RING_BUFFER_HPP
