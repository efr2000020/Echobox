#pragma once
#include "LogRecord.hpp"
#include "LogQueue.hpp"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace litespec::logging {

class JsonLineSink;

struct LoggerConfig {
    std::filesystem::path dir{"./logs"};
    std::string           baseName{"litespectrum.log"};
    LogLevel              minLevel{LogLevel::Info};
    std::size_t           maxBytesPerFile{10 * 1024 * 1024}; // 10 MiB
    unsigned int          keepFiles{5};
    // How long the consumer sleeps when the queue is empty.
    unsigned int          pollIntervalMs{20};
};

// Process-wide async logger. Producers call log(...) (cheap: format into a
// fixed-size slot, hand off to a bounded ring). A single consumer thread
// drains the ring and writes JSON-lines.
//
// Lifecycle: start() before any producer thread runs; stop() after all
// producer threads have joined. Re-entrant log() during shutdown is safe but
// records may be lost after stop() returns.
class Logger {
public:
    static Logger& instance();

    void start(const LoggerConfig& cfg);
    void stop();

    // Producer entry. Real-time safe: no allocation, mutex held only long
    // enough to copy ~256 bytes into a ring slot. Returns false if the record
    // was dropped (queue full or logger not running). Subsystem and the
    // formatted message are truncated to the LogRecord field sizes.
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
    std::thread           m_thread;
    unsigned int          m_pollIntervalMs{20};
};

} // namespace litespec::logging

// Convenience macros. Subsystem is a short string literal identifying the
// caller ("audio", "dsp", "recorder", ...).
#define LS_LOG(level, subsystem, ...) \
    ::litespec::logging::Logger::instance().log((level), (subsystem), __VA_ARGS__)
#define LS_DEBUG(subsystem, ...) LS_LOG(::litespec::logging::LogLevel::Debug, subsystem, __VA_ARGS__)
#define LS_INFO(subsystem, ...)  LS_LOG(::litespec::logging::LogLevel::Info,  subsystem, __VA_ARGS__)
#define LS_WARN(subsystem, ...)  LS_LOG(::litespec::logging::LogLevel::Warn,  subsystem, __VA_ARGS__)
#define LS_ERROR(subsystem, ...) LS_LOG(::litespec::logging::LogLevel::Error, subsystem, __VA_ARGS__)
