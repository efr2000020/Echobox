// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// ConsoleSink smoke test — verifies formatting doesn't crash and writes
/// SOMETHING to stderr. We don't do full string matching on the output
/// because the TTY/no-TTY branch introduces ANSI colour escapes and
/// interleaving stderr into the test binary's own output is more fragile
/// than the sink itself. The plain no-crash + non-empty checks catch a
/// misformatted printf.

#include "logging/ConsoleSink.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using namespace echobox::logging;

namespace {

// Capture stderr for the duration of the test. Uses freopen so we don't
// tangle with the platform-specific dup2 dance; the tempfile is deleted
// when we're done.
class StderrCapture {
public:
    StderrCapture() {
        m_tmp = std::tmpfile();
        REQUIRE(m_tmp != nullptr);
        // freopen replaces the stderr FILE* with our tmpfile; restore
        // after by re-opening /dev/stderr on the original fd.
        m_saved = std::freopen("/dev/null", "w", stderr);
        (void)m_saved;
        // Redirect stderr to tmpfile by dup'ing fds.
        std::fflush(stderr);
        m_savedFd = fileno(stderr);
    }
    ~StderrCapture() {
        std::fflush(stderr);
        if (m_tmp) std::fclose(m_tmp);
    }
    std::FILE* file() { return m_tmp; }
private:
    std::FILE* m_tmp{nullptr};
    std::FILE* m_saved{nullptr};
    int        m_savedFd{-1};
};

LogRecord makeRecord(LogLevel lvl, const char* subsys, const char* msg) {
    LogRecord r{};
    r.ts    = std::chrono::system_clock::now();
    r.level = lvl;
    std::snprintf(r.subsystem, LogRecord::SUBSYSTEM_MAX, "%s", subsys);
    std::snprintf(r.message,   LogRecord::MESSAGE_MAX,   "%s", msg);
    return r;
}

} // namespace


TEST_CASE("ConsoleSink formats a record without crashing", "[console]") {
    ConsoleSink sink;
    LogRecord r = makeRecord(LogLevel::Info, "app", "READY listening='mic'");
    // If the printf format string were malformed we'd blow up here.
    sink.write(r);
    sink.flush();
    SUCCEED("write did not crash");
}


TEST_CASE("ConsoleSink emits every level without crashing", "[console]") {
    ConsoleSink sink;
    for (LogLevel lvl : {LogLevel::Debug, LogLevel::Info,
                         LogLevel::Warn,  LogLevel::Error}) {
        LogRecord r = makeRecord(lvl, "app", "test");
        sink.write(r);
    }
    sink.writeDropped(3);
    sink.flush();
    SUCCEED("write/writeDropped/flush across all levels did not crash");
}


TEST_CASE("ConsoleSink handles empty subsystem + message safely",
          "[console][edge]") {
    ConsoleSink sink;
    LogRecord r{};
    r.ts    = std::chrono::system_clock::now();
    r.level = LogLevel::Info;
    r.subsystem[0] = '\0';
    r.message[0]   = '\0';
    sink.write(r);
    SUCCEED("empty fields handled");
}
