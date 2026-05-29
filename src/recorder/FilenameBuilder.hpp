#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>

namespace litespec::recorder {

/**
 * Builds canonical filenames for recorded events.
 *
 * Layout:
 *   <output_dir>/YYYY-MM-DD/YYYYMMDD_HHMMSS_<duration_ms>ms_<lo>-<hi>kHz.wav
 *
 * The date subdirectory keeps per-night dirs at manageable sizes when the
 * device runs unattended for long periods.
 */
class FilenameBuilder {
public:
    FilenameBuilder() = default;
    explicit FilenameBuilder(std::filesystem::path outputDir)
        : m_outputDir(std::move(outputDir)) {}

    const std::filesystem::path& outputDir() const { return m_outputDir; }

    /** Path used while the recording is in progress. Includes ".partial". */
    std::filesystem::path tempPath(std::chrono::system_clock::time_point start) const;

    /** Path the recording is renamed to on close. */
    std::filesystem::path finalPath(std::chrono::system_clock::time_point start,
                                    std::uint32_t durationMs,
                                    float loHz, float hiHz) const;

private:
    std::filesystem::path m_outputDir{"./recordings"};
};

} // namespace litespec::recorder
