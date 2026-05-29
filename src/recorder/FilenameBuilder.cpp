#include "FilenameBuilder.hpp"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace fs = std::filesystem;

namespace litespec::recorder {

namespace {

struct Parts {
    char dateDir[16];     // YYYY-MM-DD
    char timeStem[20];    // YYYYMMDD_HHMMSS
};

Parts breakdown(std::chrono::system_clock::time_point tp) {
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    Parts p{};
    std::snprintf(p.dateDir,  sizeof(p.dateDir),  "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    std::snprintf(p.timeStem, sizeof(p.timeStem), "%04d%02d%02d_%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return p;
}

} // namespace

fs::path FilenameBuilder::tempPath(std::chrono::system_clock::time_point start) const {
    Parts p = breakdown(start);
    std::string name = std::string(p.timeStem) + ".partial.wav";
    return m_outputDir / p.dateDir / name;
}

fs::path FilenameBuilder::finalPath(std::chrono::system_clock::time_point start,
                                    std::uint32_t durationMs,
                                    float loHz, float hiHz) const {
    Parts p = breakdown(start);
    char tail[48];
    const int loKhz = static_cast<int>(loHz / 1000.0f + 0.5f);
    const int hiKhz = static_cast<int>(hiHz / 1000.0f + 0.5f);
    std::snprintf(tail, sizeof(tail), "_%ums_%d-%dkHz.wav",
                  durationMs, loKhz, hiKhz);
    std::string name = std::string(p.timeStem) + tail;
    return m_outputDir / p.dateDir / name;
}

} // namespace litespec::recorder
