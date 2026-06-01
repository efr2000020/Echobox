// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// LogQueue implementation. See LogQueue.hpp for the contract.

#include "LogQueue.hpp"

namespace echobox::logging {

bool LogQueue::push(const LogRecord& r) {
    std::lock_guard lock(m_mutex);
    const std::size_t next = (m_head + 1) % CAPACITY;
    if (next == m_tail) {
        ++m_dropped;
        return false;
    }
    m_slots[m_head] = r;
    m_head = next;
    return true;
}

std::size_t LogQueue::drain(std::vector<LogRecord>& out, std::size_t& outDroppedSinceLastDrain) {
    std::lock_guard lock(m_mutex);
    std::size_t n = 0;
    while (m_tail != m_head) {
        out.push_back(m_slots[m_tail]);
        m_tail = (m_tail + 1) % CAPACITY;
        ++n;
    }
    outDroppedSinceLastDrain = m_dropped;
    m_dropped = 0;
    return n;
}

} // namespace echobox::logging
