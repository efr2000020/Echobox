// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Human-readable stderr sink for the Logger. Complements JsonLineSink
/// (which writes machine-parsable JSON-lines to a rotating file); the
/// console sink is what a field operator sees when they SSH into a running
/// unit or watch its systemd journal.

#include "LogRecord.hpp"

namespace echobox::logging {

/**
 * @brief Writes @c LogRecord values to stderr as human-readable lines.
 *
 * Format: `HH:MM:SS.mmm LEVEL [subsystem] message` (UTC timestamp).
 * When @c stderr is a TTY, level names are colourised so warnings and
 * errors stand out; otherwise plain text.
 *
 * @note Not thread-safe. Runs on the @c Logger consumer thread, same
 *       constraint as @c JsonLineSink.
 */
class ConsoleSink {
public:
    ConsoleSink();

    ConsoleSink(const ConsoleSink&)            = delete;
    ConsoleSink& operator=(const ConsoleSink&) = delete;

    void write(const LogRecord& r);
    /// Surface upstream queue drops so the operator sees they lost data.
    void writeDropped(std::size_t droppedCount);
    void flush();

private:
    bool m_colour{false};   // isatty(stderr) at construction time
};

} // namespace echobox::logging
