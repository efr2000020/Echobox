// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Session implementation. See Session.hpp for the contract.

#include "Session.hpp"

#include "logging/Logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace echobox::collection {

namespace {

// 384 kHz mono int16 = 768 000 bytes/s = 2.746 GB/h. Kept as bytes/frame *
// sampleRate so the same helper works if a caller ever configures a lower
// rate (§2.3 degradation knob: 384→192 kHz fallback).
constexpr std::uint64_t kBytesPerFrame = sizeof(std::int16_t);   // mono only

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

// Minimal JSON string escape: enough for filesystem paths, algorithm names,
// and the small config blob we already build upstream. Same posture as
// Sidecar.cpp — deliberately dependency-free.
std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
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

// Atomic file write: dump to a tmp path, fsync, rename. Same pattern as
// Sidecar so a crash mid-write never leaves half of a header on disk.
bool atomicWrite(const std::filesystem::path& target, const std::string& body) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    // create_directories returns false when the path already exists; only
    // treat a real errno as a failure.
    if (ec) {
        LS_WARN("collection", "session: mkdir %s failed: %s",
                target.parent_path().string().c_str(), ec.message().c_str());
        return false;
    }
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out) return false;
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

double estimateMaxHours(std::uint64_t freeMb, int sampleRate) {
    if (sampleRate <= 0 || freeMb == 0) return 0.0;
    const double bytesPerHour =
        static_cast<double>(sampleRate) * static_cast<double>(kBytesPerFrame) * 3600.0;
    const double freeBytes = static_cast<double>(freeMb) * 1024.0 * 1024.0;
    return freeBytes / bytesPerHour;
}

std::uint64_t availableMb(const std::filesystem::path& path) {
    std::error_code ec;
    // If the target dir doesn't yet exist, walk up to the nearest parent
    // that does. std::filesystem::space demands an existing path; the
    // collection dir might legitimately not exist at first start.
    auto probe = path;
    while (!probe.empty() && !std::filesystem::exists(probe, ec)) {
        probe = probe.parent_path();
    }
    if (probe.empty()) return 0;
    auto s = std::filesystem::space(probe, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(s.available / (1024ULL * 1024ULL));
}

Session::Session(CollectionConfig cfg, const SampleClock& clock,
                 std::filesystem::path outputDir)
    : m_cfg(std::move(cfg)),
      m_clock(clock),
      m_outputDir(std::move(outputDir)) {}

Session::~Session() {
    stop();
}

std::filesystem::path Session::headerPath() const {
    return m_outputDir / "SESSION_HEADER.json";
}

std::filesystem::path Session::endMarkerPath() const {
    return m_outputDir / "SESSION_END.json";
}

bool Session::start(const SessionMetadata& meta) {
    if (!m_cfg.enabled) return true;  // no-op when overlay is off
    if (m_started.exchange(true, std::memory_order_acq_rel)) return true;

    m_startWall   = std::chrono::system_clock::now();
    m_startSample = m_clock.now();

    const auto freeMbAtStart = availableMb(m_outputDir);
    const auto hoursAtStart  = estimateMaxHours(freeMbAtStart, meta.sampleRate);

    if (!writeHeader(meta, freeMbAtStart, hoursAtStart)) {
        LS_ERROR("collection", "session: SESSION_HEADER write failed at %s",
                 headerPath().string().c_str());
        m_started.store(false, std::memory_order_release);
        return false;
    }

    LS_INFO("collection",
            "SESSION_START dir=%s free_mb=%llu est_hours=%.2f "
            "max_duration_sec=%u min_free_mb_floor=%llu "
            "start_sample=%llu",
            m_outputDir.string().c_str(),
            static_cast<unsigned long long>(freeMbAtStart), hoursAtStart,
            m_cfg.governor.maxDurationSec,
            static_cast<unsigned long long>(m_cfg.governor.minFreeMbFloor),
            static_cast<unsigned long long>(m_startSample));

    m_running.store(true, std::memory_order_release);
    m_governor = std::thread(&Session::governorLoop, this);
    return true;
}

void Session::stop() {
    if (!m_started.load(std::memory_order_acquire)) return;
    if (m_running.exchange(false, std::memory_order_acq_rel)) {
        if (m_governor.joinable()) m_governor.join();
    }
    if (!m_endWritten.exchange(true, std::memory_order_acq_rel)) {
        writeEndMarker();
    }
}

std::string Session::stopReason() const {
    // Only meaningful after the governor has flipped stopRequested. The
    // string is written before the flag flips, so an acquire load here
    // pairs with the release in governorLoop().
    if (!m_stopRequested.load(std::memory_order_acquire)) return {};
    return m_stopReason;
}

void Session::governorLoop() {
    const auto interval = std::chrono::seconds(
        m_cfg.governor.pollIntervalSec > 0 ? m_cfg.governor.pollIntervalSec : 10);
    const auto startedAt = std::chrono::steady_clock::now();

    while (m_running.load(std::memory_order_acquire)) {
        // Sleep in short slices so stop() joins promptly on shutdown
        // without waiting a full pollInterval.
        const auto sliceMs = std::chrono::milliseconds(200);
        auto waited = std::chrono::milliseconds(0);
        while (waited < interval && m_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(sliceMs);
            waited += sliceMs;
        }
        if (!m_running.load(std::memory_order_acquire)) break;

        const auto now = std::chrono::steady_clock::now();
        const auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
            now - startedAt).count();

        if (m_cfg.governor.maxDurationSec > 0
            && static_cast<std::uint32_t>(elapsedSec) >= m_cfg.governor.maxDurationSec) {
            m_stopReason = "max-duration-reached (elapsed_sec="
                         + std::to_string(elapsedSec)
                         + ", cap_sec="
                         + std::to_string(m_cfg.governor.maxDurationSec) + ")";
            LS_INFO("collection", "SESSION_GOVERNOR_TRIP %s", m_stopReason.c_str());
            m_stopRequested.store(true, std::memory_order_release);
            break;
        }

        const auto freeMb = availableMb(m_outputDir);
        if (freeMb > 0 && freeMb < m_cfg.governor.minFreeMbFloor) {
            m_stopReason = "free-space-floor-hit (free_mb="
                         + std::to_string(freeMb)
                         + ", floor_mb="
                         + std::to_string(m_cfg.governor.minFreeMbFloor) + ")";
            LS_INFO("collection", "SESSION_GOVERNOR_TRIP %s", m_stopReason.c_str());
            m_stopRequested.store(true, std::memory_order_release);
            break;
        }
    }
}

bool Session::writeHeader(const SessionMetadata& meta,
                          std::uint64_t freeMbAtStart,
                          double hoursAtStart) const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"firmware_sha\":    \"" << jsonEscape(meta.firmwareSha)    << "\",\n";
    oss << "  \"algorithm\":       \"" << jsonEscape(meta.algorithm)      << "\",\n";
    oss << "  \"sample_rate\":     " << meta.sampleRate                     << ",\n";
    oss << "  \"channels\":        " << meta.channels                       << ",\n";
    oss << "  \"mic_device\":      \"" << jsonEscape(meta.micDevice)      << "\",\n";
    oss << "  \"site_note\":       \"" << jsonEscape(meta.siteNote)       << "\",\n";
    oss << "  \"config\":          "   << (meta.configJsonBlob.empty()
                                            ? std::string("null")
                                            : meta.configJsonBlob)         << ",\n";
    oss << "  \"start_wall_iso8601\": \"" << jsonEscape(formatIso8601Utc(m_startWall)) << "\",\n";
    oss << "  \"start_sample\":    " << m_startSample                       << ",\n";
    oss << "  \"free_mb_at_start\":" << freeMbAtStart                       << ",\n";
    oss << "  \"est_max_hours\":   " << hoursAtStart                        << ",\n";
    oss << "  \"governor\": {\n";
    oss << "    \"max_duration_sec\":   " << m_cfg.governor.maxDurationSec   << ",\n";
    oss << "    \"min_free_mb_floor\":  " << m_cfg.governor.minFreeMbFloor   << ",\n";
    oss << "    \"poll_interval_sec\":  " << m_cfg.governor.pollIntervalSec  << "\n";
    oss << "  },\n";
    oss << "  \"streams\": {\n";
    oss << "    \"a\": " << (m_cfg.streams.streamA ? "true" : "false") << ",\n";
    oss << "    \"b\": " << (m_cfg.streams.streamB ? "true" : "false") << ",\n";
    oss << "    \"c\": " << (m_cfg.streams.streamC ? "true" : "false") << ",\n";
    oss << "    \"d\": " << (m_cfg.streams.streamD ? "true" : "false") << "\n";
    oss << "  }\n";
    oss << "}\n";
    return atomicWrite(headerPath(), oss.str());
}

bool Session::writeEndMarker() const {
    const auto endWall   = std::chrono::system_clock::now();
    const auto endSample = m_clock.now();
    const auto reason    = m_stopRequested.load(std::memory_order_acquire)
                           ? m_stopReason
                           : std::string("shutdown-signal");

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"end_wall_iso8601\": \"" << jsonEscape(formatIso8601Utc(endWall)) << "\",\n";
    oss << "  \"end_sample\":       " << endSample                                << ",\n";
    oss << "  \"samples_captured\": " << (endSample >= m_startSample
                                          ? endSample - m_startSample
                                          : 0)                                     << ",\n";
    oss << "  \"reason\":           \"" << jsonEscape(reason)                     << "\"\n";
    oss << "}\n";

    if (!atomicWrite(endMarkerPath(), oss.str())) {
        LS_WARN("collection", "session: SESSION_END write failed at %s",
                endMarkerPath().string().c_str());
        return false;
    }
    LS_INFO("collection",
            "SESSION_END samples=%llu reason=%s",
            static_cast<unsigned long long>(endSample >= m_startSample
                                             ? endSample - m_startSample : 0),
            reason.c_str());
    return true;
}

} // namespace echobox::collection
