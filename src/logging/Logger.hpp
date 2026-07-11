// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Process-wide async logger. Producers format into a fixed-size slot and
/// hand off to a bounded ring; a single consumer thread writes JSON-lines.

#include "LogRecord.hpp"
#include "LogQueue.hpp"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace echobox::logging {

class JsonLineSink;
class ConsoleSink;

/// @brief Construction-time configuration for the logger.
struct LoggerConfig {
    std::filesystem::path dir{"./logs"};
    std::string           baseName{"echobox.log"};
    /// Records below this level are dropped before they reach the queue.
    LogLevel              minLevel{LogLevel::Info};
    /// Active file is rotated when it grows past this size.
    std::size_t           maxBytesPerFile{10 * 1024 * 1024}; // 10 MiB
    /// Number of rotated files to keep on disk.
    unsigned int          keepFiles{5};
    /// Consumer thread sleep when the queue is empty, in ms.
    unsigned int          pollIntervalMs{20};
    /// Mirror every record to a human-readable stderr sink alongside
    /// the JSON-lines file sink. Field operators expect to see startup /
    /// heartbeat / errors on the console when they SSH in; the file sink
    /// stays JSON for machine parsing.
    bool                  console{true};
};

/**
 * @brief Process-wide async logger.
 *
 * Producers call @c log(...) — cheap: format into a fixed-size slot, hand off
 * to a bounded ring. A single consumer thread drains the ring and writes
 * JSON-lines to a rotating file via @c JsonLineSink.
 *
 * Lifecycle: @c start() before any producer thread runs; @c stop() after all
 * producer threads have joined. Re-entrant @c log() during shutdown is safe
 * but records may be lost after @c stop() returns.
 *
 * @note Singleton.
 */
class Logger {
public:
    static Logger& instance();

    void start(const LoggerConfig& cfg);
    void stop();

    /**
     * @brief Producer entry. Real-time safe: no allocation; the queue mutex
     *        is held only long enough to copy ~256 bytes into a ring slot.
     *
     * @param level     Severity. Filtered against @c LoggerConfig::minLevel
     *                  before any work is done.
     * @param subsystem Short tag identifying the caller (@c "audio", @c "dsp",
     *                  @c "recorder", ...). Truncated to @c SUBSYSTEM_MAX.
     * @param fmt       @c printf-style format string.
     * @return @c false if the record was dropped (queue full or logger not
     *         running); otherwise @c true.
     */
    bool log(LogLevel level, const char* subsystem, const char* fmt, ...)
        __attribute__((format(printf, 4, 5)));

    LogLevel minLevel() const { return m_minLevel.load(std::memory_order_relaxed); }

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    void consumerLoop();

    std::atomic<bool>     m_running{false};
    std::atomic<LogLevel> m_minLevel{LogLevel::Info};
    LogQueue              m_queue;
    std::unique_ptr<JsonLineSink> m_sink;
    std::unique_ptr<ConsoleSink>  m_console;
    std::thread           m_thread;
    unsigned int          m_pollIntervalMs{20};
};

} // namespace echobox::logging

/// @name Convenience producer macros
/// @{
/// @c subsystem is a short string literal identifying the caller
/// (@c "audio", @c "dsp", @c "recorder", ...).
#define LS_LOG(level, subsystem, ...) \
    ::echobox::logging::Logger::instance().log((level), (subsystem), __VA_ARGS__)
#define LS_DEBUG(subsystem, ...) LS_LOG(::echobox::logging::LogLevel::Debug, subsystem, __VA_ARGS__)
#define LS_INFO(subsystem, ...)  LS_LOG(::echobox::logging::LogLevel::Info,  subsystem, __VA_ARGS__)
#define LS_WARN(subsystem, ...)  LS_LOG(::echobox::logging::LogLevel::Warn,  subsystem, __VA_ARGS__)
#define LS_ERROR(subsystem, ...) LS_LOG(::echobox::logging::LogLevel::Error, subsystem, __VA_ARGS__)
/// @}
