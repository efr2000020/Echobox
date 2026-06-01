#pragma once
/// @file
/// Log severity and the fixed-size record passed from producers to the
/// consumer thread.

#include <chrono>
#include <cstddef>

namespace echobox::logging {

/// @brief Log severity. Lower values are more verbose.
/// @c Off suppresses every record and prevents a log file from being opened.
enum class LogLevel : unsigned char {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Off   = 4,
};

/// Static lowercase name of @p lvl (@c "debug", @c "info", ...). Never null.
const char* levelName(LogLevel lvl);

/// Parse a level name into @p out. Accepts the same set @c levelName produces.
/// @return @c true on success, @c false on an unknown name (@p out unchanged).
bool        parseLevel(const char* s, LogLevel& out);

/**
 * @brief One log record passed from a producer to the consumer thread.
 *
 * Fixed-size POD so producers can fill a queue slot without allocating;
 * @c subsystem and @c message are truncated to the field sizes if the
 * producer exceeds them.
 */
struct LogRecord {
    static constexpr std::size_t SUBSYSTEM_MAX = 24;
    static constexpr std::size_t MESSAGE_MAX   = 232;

    std::chrono::system_clock::time_point ts;
    LogLevel level;
    char     subsystem[SUBSYSTEM_MAX];
    char     message[MESSAGE_MAX];
};

} // namespace echobox::logging
