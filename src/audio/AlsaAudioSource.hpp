#pragma once
#include "IAudioSource.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

struct _snd_pcm;
typedef struct _snd_pcm snd_pcm_t;

namespace echobox::audio {

/**
 * ALSA capture source. Tuned for a Dodotronic Ultramic 384K (USB-audio,
 * S16_LE, 384 kHz, mono), but works for any ALSA capture device that can
 * deliver the requested rate/format.
 *
 * Period/buffer sizes are chosen for low syscall overhead and comfortable
 * headroom against SD-card / scheduler jitter on a Pi Zero 2 W.
 */
class AlsaAudioSource final : public IAudioSource {
public:
    struct Params {
        std::string device      = "default";  // e.g. "plughw:CARD=UltraMic384K,DEV=0"
        int         sampleRate  = 384000;
        int         channels    = 1;
        unsigned    periodFrames = 4096;       // ~10.7 ms @ 384 kHz
        unsigned    bufferFrames = 16384;      // 4× period
    };

    explicit AlsaAudioSource(Params params);
    ~AlsaAudioSource() override;

    AlsaAudioSource(const AlsaAudioSource&)            = delete;
    AlsaAudioSource& operator=(const AlsaAudioSource&) = delete;

    void open() override;
    void close() noexcept override;

    int sampleRate() const override { return m_actualRate; }
    int channels()   const override { return m_params.channels; }
    const std::string& name() const override { return m_params.device; }

    int read(std::span<std::int16_t> dest) override;

private:
    bool recoverFromXrun(int err);

    Params      m_params;
    snd_pcm_t*  m_handle{nullptr};
    int         m_actualRate{0};
};

} // namespace echobox::audio
