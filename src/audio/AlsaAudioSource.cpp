// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// AlsaAudioSource implementation. See AlsaAudioSource.hpp for the contract.

#include "AlsaAudioSource.hpp"
#include "logging/Logger.hpp"

#include <alsa/asoundlib.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace echobox::audio {

AlsaAudioSource::AlsaAudioSource(Params params) : m_params(std::move(params)) {}

AlsaAudioSource::~AlsaAudioSource() {
    close();
}

void AlsaAudioSource::open() {
    if (m_handle) return;

    int err = snd_pcm_open(&m_handle, m_params.device.c_str(),
                           SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        throw std::runtime_error("alsa: open '" + m_params.device + "': "
                                 + snd_strerror(err));
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(m_handle, hw);

    if ((err = snd_pcm_hw_params_set_access(m_handle, hw,
              SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        close();
        throw std::runtime_error(std::string("alsa: set_access: ") + snd_strerror(err));
    }
    if ((err = snd_pcm_hw_params_set_format(m_handle, hw, SND_PCM_FORMAT_S16_LE)) < 0) {
        close();
        throw std::runtime_error(std::string("alsa: set_format S16_LE: ") + snd_strerror(err));
    }
    if ((err = snd_pcm_hw_params_set_channels(m_handle, hw,
              static_cast<unsigned int>(m_params.channels))) < 0) {
        close();
        throw std::runtime_error(std::string("alsa: set_channels: ") + snd_strerror(err));
    }

    unsigned int rate = static_cast<unsigned int>(m_params.sampleRate);
    int dir = 0;
    if ((err = snd_pcm_hw_params_set_rate_near(m_handle, hw, &rate, &dir)) < 0) {
        close();
        throw std::runtime_error(std::string("alsa: set_rate_near: ") + snd_strerror(err));
    }
    m_actualRate = static_cast<int>(rate);

    snd_pcm_uframes_t period = m_params.periodFrames;
    if ((err = snd_pcm_hw_params_set_period_size_near(m_handle, hw, &period, &dir)) < 0) {
        close();
        throw std::runtime_error(std::string("alsa: set_period_size_near: ") + snd_strerror(err));
    }

    snd_pcm_uframes_t buffer = m_params.bufferFrames;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(m_handle, hw, &buffer)) < 0) {
        close();
        throw std::runtime_error(std::string("alsa: set_buffer_size_near: ") + snd_strerror(err));
    }

    if ((err = snd_pcm_hw_params(m_handle, hw)) < 0) {
        close();
        throw std::runtime_error(std::string("alsa: hw_params commit: ") + snd_strerror(err));
    }

    if ((err = snd_pcm_prepare(m_handle)) < 0) {
        close();
        throw std::runtime_error(std::string("alsa: prepare: ") + snd_strerror(err));
    }

    LS_INFO("audio.alsa", "opened device='%s' rate=%d ch=%d period=%lu buffer=%lu",
            m_params.device.c_str(), m_actualRate, m_params.channels,
            static_cast<unsigned long>(period),
            static_cast<unsigned long>(buffer));

    if (m_actualRate != m_params.sampleRate) {
        LS_WARN("audio.alsa", "requested rate %d but device chose %d",
                m_params.sampleRate, m_actualRate);
    }
}

void AlsaAudioSource::close() noexcept {
    if (m_handle) {
        snd_pcm_close(m_handle);
        m_handle = nullptr;
        LS_INFO("audio.alsa", "closed");
    }
}

bool AlsaAudioSource::recoverFromXrun(int err) {
    if (err == -EPIPE) {
        LS_WARN("audio.alsa", "xrun (overrun)");
        int r = snd_pcm_prepare(m_handle);
        if (r < 0) {
            LS_ERROR("audio.alsa", "xrun recovery failed: %s", snd_strerror(r));
            return false;
        }
        return true;
    } else if (err == -ESTRPIPE) {
        LS_WARN("audio.alsa", "stream suspended; resuming");
        int r;
        while ((r = snd_pcm_resume(m_handle)) == -EAGAIN) {
            // hardware not ready yet; spin briefly
        }
        if (r < 0) {
            r = snd_pcm_prepare(m_handle);
            if (r < 0) {
                LS_ERROR("audio.alsa", "resume/prepare failed: %s", snd_strerror(r));
                return false;
            }
        }
        return true;
    }
    return false;
}

int AlsaAudioSource::read(std::span<std::int16_t> dest) {
    if (!m_handle) return -1;

    const int ch = m_params.channels;
    const snd_pcm_uframes_t wantedFrames = static_cast<snd_pcm_uframes_t>(dest.size() / ch);
    if (wantedFrames == 0) return 0;

    snd_pcm_sframes_t got = snd_pcm_readi(m_handle, dest.data(), wantedFrames);
    if (got < 0) {
        if (recoverFromXrun(static_cast<int>(got))) return 0;
        LS_ERROR("audio.alsa", "readi fatal: %s", snd_strerror(static_cast<int>(got)));
        return -1;
    }
    return static_cast<int>(got) * ch;
}

} // namespace echobox::audio
