// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// ConsoleSink implementation. See ConsoleSink.hpp for the contract.

#include "ConsoleSink.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace echobox::logging {

namespace {

// ANSI escapes for level colouring. Deliberately terse: dim/blue for
// debug, no highlight for info, yellow for warn, red for error.
const char* colourForLevel(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Debug: return "\x1b[2m";      // dim
        case LogLevel::Info:  return "";
        case LogLevel::Warn:  return "\x1b[33m";     // yellow
        case LogLevel::Error: return "\x1b[31m";     // red
        default:              return "";
    }
}

void formatTimeOfDay(std::chrono::system_clock::time_point tp,
                     char* out, std::size_t cap) {
    using namespace std::chrono;
    const auto secs   = time_point_cast<seconds>(tp);
    const auto millis = duration_cast<milliseconds>(tp - secs).count();
    const auto t      = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::snprintf(out, cap, "%02d:%02d:%02d.%03lld",
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(millis));
}

} // namespace

ConsoleSink::ConsoleSink()
    : m_colour(isatty(fileno(stderr)) != 0) {}

void ConsoleSink::write(const LogRecord& r) {
    char ts[16];
    formatTimeOfDay(r.ts, ts, sizeof(ts));

    const char* lvl = levelName(r.level);
    if (m_colour) {
        // Escape sequences are stripped from journal logs by systemd's
        // default filter, so this is safe under `journalctl -u echobox`
        // too — the operator gets colour on an interactive terminal and
        // plain text everywhere else.
        std::fprintf(stderr, "%s%s %-5s [%s] %s\x1b[0m\n",
                     colourForLevel(r.level), ts, lvl,
                     r.subsystem, r.message);
    } else {
        std::fprintf(stderr, "%s %-5s [%s] %s\n",
                     ts, lvl, r.subsystem, r.message);
    }
}

void ConsoleSink::writeDropped(std::size_t droppedCount) {
    char ts[16];
    formatTimeOfDay(std::chrono::system_clock::now(), ts, sizeof(ts));
    if (m_colour) {
        std::fprintf(stderr,
                     "\x1b[33m%s WARN  [logger] dropped %zu log records "
                     "(queue full)\x1b[0m\n",
                     ts, droppedCount);
    } else {
        std::fprintf(stderr,
                     "%s WARN  [logger] dropped %zu log records (queue full)\n",
                     ts, droppedCount);
    }
}

void ConsoleSink::flush() {
    std::fflush(stderr);
}

} // namespace echobox::logging
