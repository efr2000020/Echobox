#pragma once
#include <chrono>
#include <cstddef>

namespace echobox::logging {

enum class LogLevel : unsigned char {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Off   = 4,   // suppresses all records; no log file is opened
};

const char* levelName(LogLevel lvl);
bool        parseLevel(const char* s, LogLevel& out);

// Fixed-size POD so producers can fill a slot without allocating.
struct LogRecord {
    static constexpr std::size_t SUBSYSTEM_MAX = 24;
    static constexpr std::size_t MESSAGE_MAX   = 232;

    std::chrono::system_clock::time_point ts;
    LogLevel level;
    char     subsystem[SUBSYSTEM_MAX];
    char     message[MESSAGE_MAX];
};

} // namespace echobox::logging
