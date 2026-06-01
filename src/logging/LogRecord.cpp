// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// LogLevel name/parse helpers. See LogRecord.hpp for the enum and POD.

#include "LogRecord.hpp"
#include <cstring>

namespace echobox::logging {

const char* levelName(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Off:   return "off";
    }
    return "?";
}

bool parseLevel(const char* s, LogLevel& out) {
    if (!s) return false;
    if (std::strcmp(s, "debug") == 0) { out = LogLevel::Debug; return true; }
    if (std::strcmp(s, "info")  == 0) { out = LogLevel::Info;  return true; }
    if (std::strcmp(s, "warn")  == 0) { out = LogLevel::Warn;  return true; }
    if (std::strcmp(s, "error") == 0) { out = LogLevel::Error; return true; }
    if (std::strcmp(s, "off")   == 0) { out = LogLevel::Off;   return true; }
    if (std::strcmp(s, "none")  == 0) { out = LogLevel::Off;   return true; }
    return false;
}

} // namespace echobox::logging
