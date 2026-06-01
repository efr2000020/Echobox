// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Logger implementation. See Logger.hpp for the contract.

#include "Logger.hpp"
#include "JsonLineSink.hpp"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace echobox::logging {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    if (m_running.load(std::memory_order_acquire)) {
        stop();
    }
}

void Logger::start(const LoggerConfig& cfg) {
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return; // already running
    }
    m_minLevel.store(cfg.minLevel, std::memory_order_relaxed);
    m_pollIntervalMs = cfg.pollIntervalMs;
    m_sink = std::make_unique<JsonLineSink>(cfg.dir, cfg.baseName,
                                            cfg.maxBytesPerFile, cfg.keepFiles);
    m_thread = std::thread(&Logger::consumerLoop, this);
}

void Logger::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (m_thread.joinable()) m_thread.join();
    // Final drain in case anything landed between the last poll and stop().
    if (m_sink) {
        std::vector<LogRecord> records;
        std::size_t dropped = 0;
        m_queue.drain(records, dropped);
        for (const auto& r : records) m_sink->write(r);
        if (dropped > 0) m_sink->writeDropped(dropped);
        m_sink->flush();
        m_sink.reset();
    }
}

bool Logger::log(LogLevel level, const char* subsystem, const char* fmt, ...) {
    if (!m_running.load(std::memory_order_acquire)) return false;
    if (level < m_minLevel.load(std::memory_order_relaxed)) return false;

    LogRecord r{};
    r.ts    = std::chrono::system_clock::now();
    r.level = level;

    if (subsystem) {
        std::snprintf(r.subsystem, LogRecord::SUBSYSTEM_MAX, "%s", subsystem);
    } else {
        r.subsystem[0] = '\0';
    }

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(r.message, LogRecord::MESSAGE_MAX, fmt, args);
    va_end(args);

    return m_queue.push(r);
}

void Logger::consumerLoop() {
    std::vector<LogRecord> batch;
    batch.reserve(LogQueue::CAPACITY);

    while (m_running.load(std::memory_order_acquire)) {
        batch.clear();
        std::size_t dropped = 0;
        m_queue.drain(batch, dropped);

        if (!batch.empty() || dropped > 0) {
            for (const auto& r : batch) m_sink->write(r);
            if (dropped > 0) m_sink->writeDropped(dropped);
            m_sink->flush();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_pollIntervalMs));
        }
    }
}

} // namespace echobox::logging
