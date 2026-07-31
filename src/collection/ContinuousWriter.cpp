// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// ContinuousWriter implementation. See ContinuousWriter.hpp for the
/// contract; this TU owns the writer-thread state machine and the
/// libsndfile handle for the currently-open chunk.

#include "ContinuousWriter.hpp"

#include "logging/Logger.hpp"

#include <array>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sndfile.h>
#include <sstream>
#include <system_error>

namespace echobox::collection {

namespace {

constexpr std::size_t kDrainChunkSamples = 4096;

std::string formatIso8601Utc(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto secs   = time_point_cast<seconds>(tp);
    const auto millis = duration_cast<milliseconds>(tp - secs).count();
    const std::time_t t = system_clock::to_time_t(secs);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<long long>(millis));
    return buf;
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

} // namespace

ContinuousWriter::ContinuousWriter(ContinuousWriterConfig cfg,
                                   ReferenceRing& ring,
                                   const SampleClock& clock)
    : m_cfg(std::move(cfg)), m_ring(ring), m_clock(clock) {}

ContinuousWriter::~ContinuousWriter() { stop(); }

std::filesystem::path ContinuousWriter::manifestPath() const {
    return m_cfg.outputDir / m_cfg.manifestName;
}

void ContinuousWriter::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;
    std::error_code ec;
    std::filesystem::create_directories(m_cfg.outputDir, ec);
    if (ec) {
        LS_WARN("collection", "stream-a: mkdir %s failed: %s",
                m_cfg.outputDir.string().c_str(), ec.message().c_str());
    }
    // Anchor chunk 0 at the current sample-clock reading. Everything
    // downstream is relative to this anchor + samplesDrained; sample-level
    // alignment against Stream B is exactly this arithmetic.
    m_initialSample  = m_clock.now();
    m_samplesDrained = 0;
    LS_INFO("collection",
            "STREAM_A_START dir=%s chunk_sec=%u initial_sample=%llu",
            m_cfg.outputDir.string().c_str(),
            m_cfg.chunkDurationSec,
            static_cast<unsigned long long>(m_initialSample));
    m_thread = std::thread(&ContinuousWriter::loop, this);
}

void ContinuousWriter::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    if (m_thread.joinable()) m_thread.join();
    LS_INFO("collection",
            "STREAM_A_STOP chunks_closed=%llu drops=%llu",
            static_cast<unsigned long long>(m_chunksClosed.load()),
            static_cast<unsigned long long>(m_droppedSnapshot.load()));
}

bool ContinuousWriter::openNewChunk() {
    m_chunkStartSample = m_initialSample + m_samplesDrained;
    m_chunkStartWall   = std::chrono::system_clock::now();
    // Filename: sample-first so `ls` sorts chronologically without needing
    // to parse the wall timestamp. Wall stamp is included as a human-
    // readable secondary anchor.
    char sampleStr[32];
    std::snprintf(sampleStr, sizeof(sampleStr), "%020llu",
                  static_cast<unsigned long long>(m_chunkStartSample));
    const std::string wallStr = formatIso8601Utc(m_chunkStartWall);
    const std::string filename = std::string(sampleStr) + "_" + wallStr + ".wav";
    m_currentChunkPath = m_cfg.outputDir / filename;

    SF_INFO info{};
    info.samplerate = m_cfg.sampleRate;
    info.channels   = m_cfg.channels;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    m_currentSf = sf_open(m_currentChunkPath.string().c_str(), SFM_WRITE, &info);
    if (!m_currentSf) {
        LS_ERROR("collection", "stream-a: sf_open %s failed: %s",
                 m_currentChunkPath.string().c_str(), sf_strerror(nullptr));
        return false;
    }
    m_currentChunkFrames = 0;
    return true;
}

void ContinuousWriter::closeCurrentChunk() {
    if (!m_currentSf) return;
    sf_close(static_cast<SNDFILE*>(m_currentSf));
    m_currentSf = nullptr;

    const auto wallEnd    = std::chrono::system_clock::now();
    const std::uint64_t endSample = m_chunkStartSample + m_currentChunkFrames;
    const auto dropsNow   = m_ring.dropped();
    m_droppedSnapshot.store(dropsNow, std::memory_order_release);

    std::ostringstream rec;
    rec << "{"
        << "\"file\":\"" << jsonEscape(m_currentChunkPath.filename().string()) << "\","
        << "\"start_sample\":" << m_chunkStartSample << ","
        << "\"end_sample\":"   << endSample          << ","
        << "\"frames\":"       << m_currentChunkFrames << ","
        << "\"wall_start_iso8601\":\"" << jsonEscape(formatIso8601Utc(m_chunkStartWall)) << "\","
        << "\"wall_end_iso8601\":\""   << jsonEscape(formatIso8601Utc(wallEnd))          << "\","
        << "\"drops_snapshot\":"       << dropsNow
        << "}\n";
    appendManifest(rec.str());

    m_chunksClosed.fetch_add(1, std::memory_order_release);
}

bool ContinuousWriter::appendManifest(const std::string& jsonRecord) {
    std::ofstream out(manifestPath(), std::ios::binary | std::ios::app);
    if (!out) {
        LS_WARN("collection", "stream-a: append manifest %s failed",
                manifestPath().string().c_str());
        return false;
    }
    out.write(jsonRecord.data(), static_cast<std::streamsize>(jsonRecord.size()));
    return static_cast<bool>(out);
}

void ContinuousWriter::loop() {
    const std::uint64_t framesPerChunk =
        static_cast<std::uint64_t>(m_cfg.chunkDurationSec)
        * static_cast<std::uint64_t>(m_cfg.sampleRate);

    if (!openNewChunk()) {
        // Fatal to Stream A but not to the whole app: keep the writer
        // thread alive so a later fsync retry could succeed. In practice
        // this only fires when the collection card is unmountable.
        m_running.store(false, std::memory_order_release);
        return;
    }

    std::array<std::int16_t, kDrainChunkSamples> buf{};
    // Idle sleep: short enough that stop() joins promptly; long enough
    // that the writer isn't burning a Pi Zero core when the ring is empty.
    // 5 ms matches the recorder's poll interval so both threads share a
    // similar wake pattern.
    const auto idleSleep = std::chrono::milliseconds(5);

    auto drainOnce = [&]() -> std::size_t {
        std::size_t drained = 0;
        while (drained < buf.size()) {
            std::int16_t s;
            if (!m_ring.popOne(s)) break;
            buf[drained++] = s;
        }
        return drained;
    };
    auto writeChunkAware = [&](std::size_t drained) -> bool {
        std::size_t written = 0;
        while (written < drained) {
            const std::uint64_t remaining =
                (framesPerChunk > m_currentChunkFrames)
                ? (framesPerChunk - m_currentChunkFrames) : 0;
            const std::size_t toWrite = static_cast<std::size_t>(
                std::min<std::uint64_t>(drained - written, remaining));
            if (toWrite == 0) {
                closeCurrentChunk();
                if (!openNewChunk()) return false;
                continue;
            }
            const sf_count_t got = sf_write_short(
                static_cast<SNDFILE*>(m_currentSf),
                buf.data() + written,
                static_cast<sf_count_t>(toWrite));
            if (got != static_cast<sf_count_t>(toWrite)) {
                LS_ERROR("collection",
                         "stream-a: short write %lld/%zu on %s",
                         static_cast<long long>(got), toWrite,
                         m_currentChunkPath.string().c_str());
            }
            m_currentChunkFrames += static_cast<std::uint64_t>(got);
            m_samplesDrained     += static_cast<std::uint64_t>(got);
            written              += static_cast<std::size_t>(got);
            if (got == 0) break;   // avoid infinite loop on repeated short-writes
        }
        return true;
    };

    while (m_running.load(std::memory_order_acquire)) {
        const std::size_t drained = drainOnce();
        if (drained == 0) {
            std::this_thread::sleep_for(idleSleep);
            continue;
        }
        if (!writeChunkAware(drained)) {
            m_running.store(false, std::memory_order_release);
            break;
        }
    }
    // Final drain: whatever the producer pushed before stop() flipped
    // m_running belongs to this session and must land in the WAV, not
    // be silently discarded. The producer is guaranteed to have stopped
    // pushing before stop() joins here (Application stops the capture
    // thread first), so this loop terminates when the ring empties.
    while (true) {
        const std::size_t drained = drainOnce();
        if (drained == 0) break;
        if (!writeChunkAware(drained)) break;
    }
    closeCurrentChunk();
}

} // namespace echobox::collection
