// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// RAII wrapper around libsndfile for the recorder's WAV/PCM_16 outputs.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <sndfile.h>

namespace echobox::recorder {

/**
 * @brief RAII WAV/PCM_16 writer.
 *
 * Opens lazily on the first @c write() so a temp file is not created until
 * there is data to put in it. @c closeAndRename() finalizes the file and
 * renames it to its canonical name (close-before-rename keeps behaviour
 * consistent across filesystems).
 *
 * @note Non-copyable. One writer instance owns one open file.
 */
class WavWriter {
public:
    /**
     * @param tempPath   Working path used while writing; @c closeAndRename
     *                   moves it to the final path. Parent dirs are created
     *                   on first write.
     * @param sampleRate Sample rate stamped into the WAV header.
     * @param channels   Channel count stamped into the WAV header.
     */
    WavWriter(std::filesystem::path tempPath, int sampleRate, int channels);
    ~WavWriter();

    WavWriter(const WavWriter&)            = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    /// Write @c int16 samples (channel-interleaved for multi-channel WAVs).
    /// No-op after @c closeAndRename / @c abort.
    void write(std::span<const std::int16_t> samples);

    /// Total frames (samples per channel) written so far.
    std::uint64_t framesWritten() const { return m_framesWritten; }

    /**
     * @brief Close the file and rename @c tempPath to @p finalPath.
     * Idempotent: subsequent calls are no-ops. Falls back to copy+remove if
     * @p finalPath is on a different filesystem from @c tempPath.
     */
    void closeAndRename(const std::filesystem::path& finalPath);

    /// Close and delete the temp file without producing a final file.
    void abort();

private:
    void openIfNeeded();

    std::filesystem::path m_tempPath;
    int                   m_sampleRate;
    int                   m_channels;
    SNDFILE*              m_sf{nullptr};
    std::uint64_t         m_framesWritten{0};
    bool                  m_closed{false};
};

} // namespace echobox::recorder
