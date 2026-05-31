#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>

namespace echobox::recorder {

/**
 * Builds canonical filenames for recorded events.
 *
 * Layout:
 *   <output_dir>/YYYY-MM-DD/YYYYMMDD_HHMMSSsss_<duration_ms>ms.wav
 *
 * The time stem is a single fixed-width 9-digit field with millisecond
 * resolution (HHMMSSsss). Recordings can fire several times per second on a
 * busy night, so second-level resolution previously risked collisions; the
 * ms suffix removes that risk while still sorting cleanly as plain text.
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
                                    std::uint32_t durationMs) const;

private:
    std::filesystem::path m_outputDir{"./recordings"};
};

} // namespace echobox::recorder
