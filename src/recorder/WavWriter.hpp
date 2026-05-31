#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <sndfile.h>

namespace echobox::recorder {

/**
 * Tiny RAII wrapper around libsndfile for WAV/PCM_16 writes.
 *
 * Opens lazily on the first write so the file is not created until we are
 * actually going to put samples in it. closeAndRename() finalizes the file
 * and atomically renames it to its final name (file is closed before rename
 * to keep behavior consistent across filesystems).
 */
class WavWriter {
public:
    WavWriter(std::filesystem::path tempPath, int sampleRate, int channels);
    ~WavWriter();

    WavWriter(const WavWriter&)            = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    /** Writes int16 samples (channels-interleaved if multi-channel). */
    void write(std::span<const std::int16_t> samples);

    /** Total frames (samples per channel) written so far. */
    std::uint64_t framesWritten() const { return m_framesWritten; }

    /** Close + rename. Idempotent: subsequent calls are no-ops. */
    void closeAndRename(const std::filesystem::path& finalPath);

    /** Close + delete the temp file without producing a final file. */
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
