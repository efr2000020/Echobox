// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Bounded MPSC ring buffer for log records.

#include "LogRecord.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace echobox::logging {

/**
 * @brief Bounded multi-producer / single-consumer log-record queue.
 *
 * Producers acquire a short mutex — only to copy a ~256-byte record into a
 * ring slot — and never do I/O inside the lock. The single consumer thread
 * drains in bulk under the same lock.
 *
 * @note Hot-path producers (DSP / audio threads) call @c push() directly. A
 *       full-queue event causes the record to be dropped and counted; the
 *       caller is never blocked.
 */
class LogQueue {
public:
    static constexpr std::size_t CAPACITY = 1024;

    /// Producer entry. @return @c true if accepted, @c false if dropped
    /// because the queue was full at the moment of the call.
    bool push(const LogRecord& r);

    /**
     * @brief Consumer entry. Move all pending records into @p out (appended).
     * @param out  Destination, appended to.
     * @param outDroppedSinceLastDrain Out: number of records dropped since
     *             the last drain, so the consumer can surface the loss.
     * @return Number of records drained this call.
     */
    std::size_t drain(std::vector<LogRecord>& out, std::size_t& outDroppedSinceLastDrain);

private:
    std::array<LogRecord, CAPACITY> m_slots{};
    std::size_t m_head{0};            // next write index
    std::size_t m_tail{0};            // next read index
    std::size_t m_dropped{0};
    std::mutex  m_mutex;
};

} // namespace echobox::logging
