// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Capture-side audio source interface. Implementations bridge a concrete
/// backend (ALSA today; CoreAudio / a file replayer in tests) to the rest of
/// the pipeline.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace echobox::audio {

/**
 * @brief Capture-side audio source.
 *
 * Samples are returned as native @c int16, mono interleaved. @c channels()
 * returning @c 1 is the only configuration the production binary supports;
 * the interface is shaped to allow multichannel later without a breaking
 * change.
 *
 * Lifecycle: the owning thread calls @c open() once, then @c read() in a
 * loop until @c close(). Implementations should block @c read() for up to
 * a short timeout (~100 ms) when no samples are immediately available and
 * return @c 0 in that case so the caller can re-check its shutdown flag.
 *
 * @note Not thread-safe. One thread owns the source for its entire lifetime.
 */
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    /**
     * @brief Open the underlying device.
     * @throws std::runtime_error on any backend failure (device missing, busy,
     *         unsupported format, etc.). Message includes the backend's error.
     */
    virtual void open() = 0;

    /// Close the device. Idempotent and noexcept; safe in destructors.
    virtual void close() noexcept = 0;

    /// Actual sample rate the device negotiated, in Hz. Valid after @c open().
    virtual int sampleRate() const = 0;
    /// Channel count the device negotiated. Valid after @c open().
    virtual int channels()   const = 0;

    /// Human-readable identifier for log lines.
    virtual const std::string& name() const = 0;

    /**
     * @brief Block-read up to @c dest.size() interleaved samples.
     * @param dest Output buffer.
     * @return  @c >0 : number of @c int16 samples written to @c dest;
     *          @c ==0: no samples this call (short timeout); not an error;
     *          @c <0 : fatal — source is no longer usable; caller must stop.
     */
    virtual int read(std::span<std::int16_t> dest) = 0;
};

} // namespace echobox::audio
