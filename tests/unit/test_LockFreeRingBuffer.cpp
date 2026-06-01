// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "../../src/common/LockFreeRingBuffer.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

TEST_CASE("LockFreeRingBuffer Initial State", "[unit][buffer]") {
    LockFreeRingBuffer<int> rb(10);
    REQUIRE(rb.available_read() == 0);
    int val = 0;
    REQUIRE(rb.pop(val) == false);
}

TEST_CASE("LockFreeRingBuffer Full/Empty Behavior", "[unit][buffer]") {
    const size_t capacity = 5;
    LockFreeRingBuffer<int> rb(capacity);

    SECTION("Filling the buffer") {
        for (int i = 0; i < (int)capacity; ++i) {
            REQUIRE(rb.push(i) == true);
        }
        REQUIRE(rb.available_read() == capacity);
        REQUIRE(rb.push(99) == false);
    }

    SECTION("Emptying the buffer") {
        for (int i = 0; i < (int)capacity; ++i) rb.push(i);
        
        for (int i = 0; i < (int)capacity; ++i) {
            int val = -1;
            REQUIRE(rb.pop(val) == true);
            REQUIRE(val == i);
        }
        REQUIRE(rb.available_read() == 0);
        int final_val;
        REQUIRE(rb.pop(final_val) == false);
    }
}

TEST_CASE("LockFreeRingBuffer Wrap Around Logic", "[unit][buffer]") {
    const size_t capacity = 4;
    LockFreeRingBuffer<int> rb(capacity);

    // Push 3, Pop 3 (moving pointers forward)
    for (int i = 0; i < 3; ++i) rb.push(i);
    for (int i = 0; i < 3; ++i) { int v; rb.pop(v); }

    // Now head/tail are at index 3. Push 3 more to force wrap around.
    for (int i = 10; i < 13; ++i) {
        REQUIRE(rb.push(i) == true);
    }
    
    REQUIRE(rb.available_read() == 3);

    for (int i = 10; i < 13; ++i) {
        int val = 0;
        REQUIRE(rb.pop(val) == true);
        REQUIRE(val == i);
    }
}

TEST_CASE("LockFreeRingBuffer Capacity One", "[unit][buffer]") {
    LockFreeRingBuffer<int> rb(1);
    REQUIRE(rb.push(42) == true);
    REQUIRE(rb.push(43) == false);
    int val;
    REQUIRE(rb.pop(val) == true);
    REQUIRE(val == 42);
}

TEST_CASE("LockFreeRingBuffer Concurrency Stress", "[stress][buffer]") {
    const long long iterations = 100000; // Reduced for unit tests, can be increased for dedicated stress tests
    const size_t buffer_size = 1024;
    LockFreeRingBuffer<long long> rb(buffer_size);
    
    std::atomic<bool> start_signal{false};
    std::atomic<long long> pushed_count{0};
    std::atomic<long long> popped_count{0};
    std::atomic<bool> integrity_fail{false};

    auto producer = [&]() {
        while (!start_signal.load()) std::this_thread::yield();
        for (long long i = 0; i < iterations; ++i) {
            while (!rb.push(i)) {
                std::this_thread::yield();
            }
            pushed_count++;
        }
    };

    auto consumer = [&]() {
        while (!start_signal.load()) std::this_thread::yield();
        while (popped_count.load() < iterations) {
            long long val;
            if (rb.pop(val)) {
                if (val != popped_count.load()) {
                    integrity_fail.store(true);
                    return;
                }
                popped_count++;
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::thread t1(producer);
    std::thread t2(consumer);

    start_signal.store(true);
    
    t1.join();
    t2.join();

    REQUIRE(integrity_fail.load() == false);
    REQUIRE(popped_count.load() == iterations);
}
