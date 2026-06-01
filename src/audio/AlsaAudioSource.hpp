// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// ALSA implementation of IAudioSource. Forward-declares the libasound types
/// so consumers don't pick up the full @c <alsa/asoundlib.h> just by
/// including the audio source header.

#include "IAudioSource.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

struct _snd_pcm;
typedef struct _snd_pcm snd_pcm_t;

namespace echobox::audio {

/**
 * @brief ALSA capture source.
 *
 * Tuned for the Dodotronic Ultramic 384K (USB-audio, S16_LE, 384 kHz, mono),
 * but works for any ALSA capture device that can deliver the requested
 * rate/format. Period and buffer sizes are sized for low syscall overhead
 * with comfortable headroom against SD-card and scheduler jitter on a
 * Pi Zero 2 W.
 */
class AlsaAudioSource final : public IAudioSource {
public:
    /// Construction-time parameters. Defaults match a stock UltraMic 384K.
    struct Params {
        /// ALSA device string, e.g. @c "plughw:CARD=UltraMic384K,DEV=0".
        std::string device      = "default";
        /// Requested sample rate. The device may negotiate a near rate;
        /// inspect @c sampleRate() after @c open() for the actual value.
        int         sampleRate  = 384000;
        int         channels    = 1;
        /// ALSA period (interrupt cadence) in frames. ~10.7 ms @ 384 kHz.
        unsigned    periodFrames = 4096;
        /// ALSA ring buffer in frames. 4× period leaves headroom for jitter.
        unsigned    bufferFrames = 16384;
    };

    explicit AlsaAudioSource(Params params);
    ~AlsaAudioSource() override;

    AlsaAudioSource(const AlsaAudioSource&)            = delete;
    AlsaAudioSource& operator=(const AlsaAudioSource&) = delete;

    /// @copydoc IAudioSource::open
    /// @throws std::runtime_error on any ALSA error during open or configure.
    void open() override;
    void close() noexcept override;

    int sampleRate() const override { return m_actualRate; }
    int channels()   const override { return m_params.channels; }
    const std::string& name() const override { return m_params.device; }

    int read(std::span<std::int16_t> dest) override;

private:
    /// Attempt to recover from an @c -EPIPE (overrun) or @c -ESTRPIPE (stream
    /// suspended). Returns @c true if the stream is usable again, @c false if
    /// the error was unrecoverable or unrelated.
    bool recoverFromXrun(int err);

    Params      m_params;
    snd_pcm_t*  m_handle{nullptr};
    int         m_actualRate{0};
};

} // namespace echobox::audio
