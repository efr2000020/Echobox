// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// WavFileAudioSource implementation. See WavFileAudioSource.hpp.

#include "WavFileAudioSource.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <sndfile.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace echobox::replay {

namespace {

inline SNDFILE* asSf(void* h) { return static_cast<SNDFILE*>(h); }

} // namespace

std::vector<fs::path> enumerateWavFiles(const fs::path& p) {
    if (!fs::exists(p)) {
        throw std::runtime_error("replay: path does not exist: " + p.string());
    }

    std::vector<fs::path> out;
    if (fs::is_regular_file(p)) {
        if (p.extension() == ".wav" || p.extension() == ".WAV") {
            out.push_back(p);
        }
    } else if (fs::is_directory(p)) {
        for (const auto& entry : fs::recursive_directory_iterator(p)) {
            if (!entry.is_regular_file()) continue;
            const auto ext = entry.path().extension();
            if (ext == ".wav" || ext == ".WAV") out.push_back(entry.path());
        }
    } else {
        throw std::runtime_error("replay: path is neither file nor directory: " + p.string());
    }

    if (out.empty()) {
        throw std::runtime_error("replay: no .wav files under " + p.string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

WavFileAudioSource::WavFileAudioSource(Params params)
    : m_params(std::move(params)),
      m_sampleRate(m_params.expectedSampleRate),
      m_name("(unopened)") {}

WavFileAudioSource::~WavFileAudioSource() {
    close();
}

void WavFileAudioSource::openCurrent() {
    SF_INFO info{};
    SNDFILE* sf = sf_open(m_params.files[m_fileIdx].c_str(), SFM_READ, &info);
    if (!sf) {
        throw std::runtime_error(
            "replay: sf_open failed for '" + m_params.files[m_fileIdx].string()
            + "': " + sf_strerror(nullptr));
    }
    if (info.channels != m_params.expectedChannels) {
        std::ostringstream os;
        os << "replay: '" << m_params.files[m_fileIdx].string()
           << "' has channels=" << info.channels
           << " but expected " << m_params.expectedChannels;
        sf_close(sf);
        throw std::runtime_error(os.str());
    }
    if (m_fileIdx == 0) {
        m_sampleRate = info.samplerate;
    } else if (info.samplerate != m_sampleRate) {
        std::ostringstream os;
        os << "replay: '" << m_params.files[m_fileIdx].string()
           << "' has sample_rate=" << info.samplerate
           << " but expected " << m_sampleRate
           << " (first file in the playlist set the rate)";
        sf_close(sf);
        throw std::runtime_error(os.str());
    }
    m_current = sf;
    m_name = m_params.files[m_fileIdx].filename().string();
    LS_INFO("replay.wav", "FILE_OPEN idx=%zu/%zu name='%s' sr=%d ch=%d frames=%lld",
            m_fileIdx + 1, m_params.files.size(),
            m_name.c_str(), info.samplerate, info.channels,
            static_cast<long long>(info.frames));
}

void WavFileAudioSource::closeCurrent() noexcept {
    if (m_current) {
        sf_close(asSf(m_current));
        m_current = nullptr;
    }
}

void WavFileAudioSource::open() {
    if (m_current) return;
    if (m_params.files.empty()) {
        throw std::runtime_error("replay: playlist is empty");
    }
    m_fileIdx  = 0;
    m_finished = false;
    openCurrent();
    if (m_sampleRate != m_params.expectedSampleRate) {
        // Not fatal; the driver will pick up the actual rate via
        // sampleRate() and reconfigure DspPipelineConfig. Surface it
        // so a misconfigured CLI doesn't silently produce sample-rate-
        // mismatched sidecars.
        LS_WARN("replay.wav",
                "playlist sample rate %d differs from CLI --sample-rate %d; "
                "using playlist rate",
                m_sampleRate, m_params.expectedSampleRate);
    }
}

void WavFileAudioSource::close() noexcept {
    closeCurrent();
    m_finished = true;
}

int WavFileAudioSource::read(std::span<std::int16_t> dest) {
    if (m_finished) return 0;
    if (!m_current) return 0;

    const int ch = m_params.expectedChannels;
    const sf_count_t wantedFrames = static_cast<sf_count_t>(dest.size() / ch);
    if (wantedFrames == 0) return 0;

    sf_count_t got = sf_readf_short(asSf(m_current), dest.data(), wantedFrames);
    if (got > 0) {
        return static_cast<int>(got) * ch;
    }

    // Current file is drained — roll to the next one, if any. On EOF of
    // the whole playlist, mark finished and return 0 (idle) so the
    // replay driver can stop cleanly without a spurious MIC_DISCONNECTED.
    closeCurrent();
    ++m_fileIdx;
    if (m_fileIdx >= m_params.files.size()) {
        m_finished = true;
        LS_INFO("replay.wav", "PLAYLIST_DONE files=%zu", m_params.files.size());
        return 0;
    }
    try {
        openCurrent();
    } catch (const std::exception& e) {
        LS_ERROR("replay.wav", "FILE_OPEN failed on rollover: %s", e.what());
        m_finished = true;
        return 0;
    }
    // Return 0 for this call; the next read() will pull from the newly
    // opened file. Keeps the read() contract simple ("one file per call")
    // even at rollover boundaries.
    return 0;
}

} // namespace echobox::replay
