#pragma once
#include "LogRecord.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace litespec::logging {

// Bounded MPSC queue. Producers acquire a short mutex (only to copy a
// ~256-byte record into a ring slot — no I/O is ever done inside the lock).
// The single consumer thread drains in bulk by swapping under the same lock.
//
// The DSP / audio threads call push() on the hot path. A queue full event
// causes the record to be dropped and counted — never blocks the caller.
class LogQueue {
public:
    static constexpr std::size_t CAPACITY = 1024;

    // Returns true if accepted, false if dropped (queue full).
    bool push(const LogRecord& r);

    // Move all pending records into `out` (appended). Returns the number drained.
    // Also returns the number of records dropped since the last drain via
    // `outDroppedSinceLastDrain` so the consumer can surface the loss.
    std::size_t drain(std::vector<LogRecord>& out, std::size_t& outDroppedSinceLastDrain);

private:
    std::array<LogRecord, CAPACITY> m_slots{};
    std::size_t m_head{0};            // next write index
    std::size_t m_tail{0};            // next read index
    std::size_t m_dropped{0};
    std::mutex  m_mutex;
};

} // namespace litespec::logging
