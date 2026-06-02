// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// FilenameBuilder implementation. See FilenameBuilder.hpp for the layout.

#include "FilenameBuilder.hpp"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace fs = std::filesystem;

namespace echobox::recorder {

namespace {

struct Parts {
    char dateDir[16];     // YYYY-MM-DD
    char timeStem[24];    // YYYYMMDD_HHMMSSsss
};

Parts breakdown(std::chrono::system_clock::time_point tp) {
    // Pull the millisecond component out before truncating to seconds, so the
    // ms field is independent of the snapshot the local-time conversion sees.
    using namespace std::chrono;
    const auto sinceEpoch = tp.time_since_epoch();
    const auto secs       = duration_cast<seconds>(sinceEpoch);
    const auto ms         = duration_cast<milliseconds>(sinceEpoch - secs).count();

    const auto t = system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    Parts p{};
    std::snprintf(p.dateDir,  sizeof(p.dateDir),  "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    std::snprintf(p.timeStem, sizeof(p.timeStem), "%04d%02d%02d_%02d%02d%02d%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms));
    return p;
}

} // namespace

fs::path FilenameBuilder::tempPath(std::chrono::system_clock::time_point start) const {
    Parts p = breakdown(start);
    std::string name = std::string(p.timeStem) + ".partial.wav";
    return m_outputDir / p.dateDir / name;
}

fs::path FilenameBuilder::finalPath(std::chrono::system_clock::time_point start) const {
    Parts p = breakdown(start);
    std::string name = std::string(p.timeStem) + ".wav";
    return m_outputDir / p.dateDir / name;
}

} // namespace echobox::recorder
