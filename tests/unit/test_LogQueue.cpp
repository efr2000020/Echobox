#include "logging/LogQueue.hpp"
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <thread>
#include <atomic>

using namespace echobox::logging;

TEST_CASE("LogQueue: basic operations", "[logging]") {
    LogQueue queue;
    std::vector<LogRecord> out;
    std::size_t dropped = 0;

    SECTION("Empty queue") {
        CHECK(queue.drain(out, dropped) == 0);
        CHECK(dropped == 0);
        CHECK(out.empty());
    }

    SECTION("Single push and drain") {
        LogRecord r;
        r.level = LogLevel::Info;
        std::snprintf(r.message, LogRecord::MESSAGE_MAX, "Hello");
        
        CHECK(queue.push(r));
        CHECK(queue.drain(out, dropped) == 1);
        CHECK(dropped == 0);
        REQUIRE(out.size() == 1);
        CHECK(std::string(out[0].message) == "Hello");
        
        // Next drain should be empty
        out.clear();
        CHECK(queue.drain(out, dropped) == 0);
    }
}

TEST_CASE("LogQueue: capacity and drops", "[logging]") {
    LogQueue queue;
    std::vector<LogRecord> out;
    std::size_t dropped = 0;

    SECTION("Fill to capacity") {
        // Implementation leaves one slot empty to distinguish full/empty
        const std::size_t effectiveCap = LogQueue::CAPACITY - 1;
        for (std::size_t i = 0; i < effectiveCap; ++i) {
            LogRecord r;
            CHECK(queue.push(r));
        }
        
        // Next push should fail
        LogRecord overflow;
        CHECK_FALSE(queue.push(overflow));
        
        CHECK(queue.drain(out, dropped) == effectiveCap);
        CHECK(dropped == 1);
        CHECK(out.size() == effectiveCap);
    }
}

TEST_CASE("LogQueue: multi-producer", "[logging]") {
    LogQueue queue;
    std::atomic<bool> start{false};
    const int numProducers = 4;
    const int recordsPerProducer = 100;
    
    std::vector<std::thread> producers;
    for (int i = 0; i < numProducers; ++i) {
        producers.emplace_back([&, i]() {
            while (!start) std::this_thread::yield();
            for (int j = 0; j < recordsPerProducer; ++j) {
                LogRecord r;
                r.level = LogLevel::Debug;
                std::snprintf(r.message, LogRecord::MESSAGE_MAX, "P%d-R%d", i, j);
                queue.push(r);
            }
        });
    }

    start = true;
    for (auto& t : producers) t.join();

    std::vector<LogRecord> out;
    std::size_t dropped = 0;
    queue.drain(out, dropped);
    
    // Total records should be numProducers * recordsPerProducer (unless dropped, but capacity is 1024 > 400)
    CHECK(out.size() == static_cast<std::size_t>(numProducers * recordsPerProducer));
    CHECK(dropped == 0);
}
