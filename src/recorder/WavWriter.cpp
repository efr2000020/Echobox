// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// WavWriter implementation. See WavWriter.hpp for the public contract.

#include "WavWriter.hpp"
#include "logging/Logger.hpp"

#include <sndfile.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace echobox::recorder {

WavWriter::WavWriter(fs::path tempPath, int sampleRate, int channels)
    : m_tempPath(std::move(tempPath)),
      m_sampleRate(sampleRate),
      m_channels(channels) {}

WavWriter::~WavWriter() {
    if (!m_closed) {
        abort();
    }
}

void WavWriter::openIfNeeded() {
    if (m_sf || m_closed) return;

    std::error_code ec;
    fs::create_directories(m_tempPath.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("wav: mkdir " + m_tempPath.parent_path().string()
                                 + ": " + ec.message());
    }

    SF_INFO info{};
    info.samplerate = m_sampleRate;
    info.channels   = m_channels;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    m_sf = sf_open(m_tempPath.c_str(), SFM_WRITE, &info);
    if (!m_sf) {
        throw std::runtime_error("wav: sf_open " + m_tempPath.string()
                                 + ": " + sf_strerror(nullptr));
    }
}

void WavWriter::write(std::span<const std::int16_t> samples) {
    if (m_closed || samples.empty()) return;
    openIfNeeded();

    const sf_count_t frames = static_cast<sf_count_t>(samples.size() / m_channels);
    const sf_count_t wrote  = sf_writef_short(m_sf, samples.data(), frames);
    if (wrote != frames) {
        LS_ERROR("recorder.wav", "short write: wanted %lld got %lld: %s",
                 static_cast<long long>(frames), static_cast<long long>(wrote),
                 sf_strerror(m_sf));
    }
    m_framesWritten += static_cast<std::uint64_t>(wrote);
}

void WavWriter::closeAndRename(const fs::path& finalPath) {
    if (m_closed) return;
    if (m_sf) {
        sf_close(m_sf);
        m_sf = nullptr;
    }
    m_closed = true;

    if (!fs::exists(m_tempPath)) return;  // nothing written

    std::error_code ec;
    fs::create_directories(finalPath.parent_path(), ec);
    fs::rename(m_tempPath, finalPath, ec);
    if (ec) {
        // Cross-filesystem rename? Fall back to copy+remove.
        fs::copy_file(m_tempPath, finalPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LS_ERROR("recorder.wav", "rename/copy failed: %s", ec.message().c_str());
            return;
        }
        fs::remove(m_tempPath, ec);
    }
}

void WavWriter::abort() {
    if (m_closed) return;
    if (m_sf) {
        sf_close(m_sf);
        m_sf = nullptr;
    }
    m_closed = true;
    std::error_code ec;
    fs::remove(m_tempPath, ec);
}

} // namespace echobox::recorder
