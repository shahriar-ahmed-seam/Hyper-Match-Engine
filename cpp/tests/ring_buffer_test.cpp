// Unit tests for the fixed-capacity Ring_Buffer.
//
// These verify the FIFO ordering and the full -> reject back-pressure contract
// that preserves buffered elements. The numbered correctness property for
// back-pressure is covered separately by the RapidCheck property test.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "hme/ring_buffer.hpp"

using hme::engine::RingBuffer;

TEST_CASE("RingBuffer reports its fixed capacity and starts empty", "[ring]") {
    RingBuffer<int, 4> rb;
    CHECK(RingBuffer<int, 4>::capacity() == 4);
    CHECK(rb.size() == 0);
    CHECK(rb.empty());
    CHECK_FALSE(rb.full());
}

TEST_CASE("RingBuffer dequeues in first-in-first-out order", "[ring][fifo]") {
    RingBuffer<int, 8> rb;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(rb.try_push(i));
    }
    CHECK(rb.size() == 5);

    for (int i = 0; i < 5; ++i) {
        int out = -1;
        REQUIRE(rb.try_pop(out));
        CHECK(out == i);  // FIFO: same order as pushed.
    }
    CHECK(rb.empty());
}

TEST_CASE("RingBuffer try_pop on an empty buffer returns false", "[ring]") {
    RingBuffer<int, 2> rb;
    int out = 42;
    CHECK_FALSE(rb.try_pop(out));
    CHECK(out == 42);  // out left unchanged.
}

TEST_CASE("RingBuffer fills to capacity then rejects with back-pressure", "[ring][backpressure]") {
    RingBuffer<int, 3> rb;
    REQUIRE(rb.try_push(10));
    REQUIRE(rb.try_push(20));
    REQUIRE(rb.try_push(30));
    REQUIRE(rb.full());
    CHECK(rb.size() == 3);

    // Full -> reject (back-pressure), buffered elements preserved.
    CHECK_FALSE(rb.try_push(40));
    CHECK_FALSE(rb.try_push(50));
    CHECK(rb.size() == 3);

    // The original three elements are intact and still in FIFO order.
    int out = 0;
    REQUIRE(rb.try_pop(out));
    CHECK(out == 10);
    REQUIRE(rb.try_pop(out));
    CHECK(out == 20);
    REQUIRE(rb.try_pop(out));
    CHECK(out == 30);
    CHECK(rb.empty());
}

TEST_CASE("RingBuffer wraps around the circular storage correctly", "[ring][wrap]") {
    RingBuffer<int, 3> rb;
    // Fill, drain partially, refill so head/tail wrap past the array end.
    REQUIRE(rb.try_push(1));
    REQUIRE(rb.try_push(2));
    REQUIRE(rb.try_push(3));

    int out = 0;
    REQUIRE(rb.try_pop(out));  // removes 1
    CHECK(out == 1);
    REQUIRE(rb.try_pop(out));  // removes 2
    CHECK(out == 2);

    REQUIRE(rb.try_push(4));   // wraps into freed slot
    REQUIRE(rb.try_push(5));   // wraps into freed slot
    REQUIRE(rb.full());
    CHECK_FALSE(rb.try_push(6));  // full again -> back-pressure

    REQUIRE(rb.try_pop(out));
    CHECK(out == 3);
    REQUIRE(rb.try_pop(out));
    CHECK(out == 4);
    REQUIRE(rb.try_pop(out));
    CHECK(out == 5);
    CHECK(rb.empty());
}

TEST_CASE("RingBuffer supports move-only-friendly types via try_push overloads", "[ring][move]") {
    RingBuffer<std::string, 2> rb;
    std::string a = "hello";
    REQUIRE(rb.try_push(a));             // copy overload
    REQUIRE(rb.try_push(std::string{"world"}));  // move overload
    REQUIRE(rb.full());
    CHECK_FALSE(rb.try_push(std::string{"overflow"}));

    std::string out;
    REQUIRE(rb.try_pop(out));
    CHECK(out == "hello");
    REQUIRE(rb.try_pop(out));
    CHECK(out == "world");
}
