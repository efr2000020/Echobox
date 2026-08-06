// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// File-backed IAudioSource: streams a list of WAV files into the same
/// capture loop the microphone feeds. Replay-tool only; never compiled
/// into the shipping binary. The IAudioSource header calls "a file
/// replayer" out as an intended implementation, so this is the designed
/// seam — no shape change to the interface.
///
/// Semantics that differ from ALSA:
///   - No timing: each read() returns as many samples as the caller asks
///     for, back-to-back, until the current file is exhausted. When the
///     current file ends and a next file is available, the source rolls
///     over silently on the next read(). When the whole playlist is
///     exhausted, read() returns 0 (idle) forever and @c finished() is
///     true — the replay driver uses that flag to stop cleanly instead
///     of treating EOF as a fatal MIC_DISCONNECTED.
///   - All files in the playlist must share the sample rate the caller
///     opened the source with (i.e. the CLI --sample-rate); a mismatch
///     is fatal at open() time so a bad mix is caught up front rather
///     than silently mis-clocking the pipeline halfway through.

#include "audio/IAudioSource.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace echobox::replay {

/// Enumerate a directory (recursive) or accept a single WAV path, sorted
/// lexicographically so replay runs are reproducible. Non-@c .wav files
/// are silently skipped. Throws @c std::runtime_error if the path does
/// not exist or the resulting list is empty.
std::vector<std::filesystem::path> enumerateWavFiles(const std::filesystem::path& p);

/**
 * @brief IAudioSource that streams a fixed playlist of WAV files.
 *
 * Not thread-safe; the same thread that owns an @c AlsaAudioSource owns
 * this instance.
 */
class WavFileAudioSource final : public echobox::audio::IAudioSource {
public:
    struct Params {
        /// Playlist in play order. Must be non-empty at open() time.
        std::vector<std::filesystem::path> files;
        /// Expected sample rate; every file must match. open() throws
        /// std::runtime_error if any file's header disagrees.
        int expectedSampleRate{384000};
        /// Expected channel count. Only 1 is meaningful for Echobox today;
        /// kept explicit so a stereo WAV in the corpus fails loudly instead
        /// of silently doubling the DSP hop rate.
        int expectedChannels{1};
    };

    explicit WavFileAudioSource(Params params);
    ~WavFileAudioSource() override;

    WavFileAudioSource(const WavFileAudioSource&)            = delete;
    WavFileAudioSource& operator=(const WavFileAudioSource&) = delete;

    void open() override;
    void close() noexcept override;

    int sampleRate()          const override { return m_sampleRate; }
    int channels()            const override { return m_params.expectedChannels; }
    const std::string& name() const override { return m_name; }

    /// @return @c >0 samples read, @c 0 when the whole playlist is drained
    ///         (never returns @c -1 — EOF isn't fatal for replay).
    int read(std::span<std::int16_t> dest) override;

    /// True once every file has been read to completion.
    bool finished() const { return m_finished; }

    /// Index of the currently-open file, in the same order as @c files.
    /// Zero-based; equal to @c files.size() once @c finished() is true.
    std::size_t currentFileIndex() const { return m_fileIdx; }
    /// Total files in the playlist.
    std::size_t fileCount() const { return m_params.files.size(); }

private:
    /// Open the file at @c m_fileIdx (assumed in range) into @c m_current.
    /// Sets @c m_sampleRate on the first file; validates against subsequent
    /// files. Throws @c std::runtime_error on any libsndfile error.
    void openCurrent();
    /// Close @c m_current if open. Idempotent.
    void closeCurrent() noexcept;

    Params      m_params;
    /// Opaque libsndfile handle (SNDFILE*); typed void* so the header
    /// doesn't drag <sndfile.h> into every translation unit that
    /// merely knows the source exists.
    void*       m_current{nullptr};
    std::size_t m_fileIdx{0};
    int         m_sampleRate{0};
    bool        m_finished{false};
    /// Human-readable identifier used by log lines; updated as the source
    /// rolls between files so log context tracks the file under the head.
    std::string m_name;
};

} // namespace echobox::replay
