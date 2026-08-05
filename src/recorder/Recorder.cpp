// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Recorder implementation. See Recorder.hpp for the contract; this TU also
/// holds the temporal state machine that drives the open/append/close cycle.

#include "Recorder.hpp"
#include "Sidecar.hpp"
#include "WavWriter.hpp"
#include "logging/Logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace echobox::recorder {

namespace {
constexpr std::size_t kDrainChunkSamples = 4096;

std::uint64_t framesForMs(std::uint32_t ms, int sampleRate) {
    return static_cast<std::uint64_t>(ms) * static_cast<std::uint64_t>(sampleRate) / 1000ULL;
}

// Near-miss margin around the primary sweep-shape gate threshold. Events
// whose 10-dB bandwidth sits within ± this many kHz of the tunable
// min_bandwidth_khz qualify as "boundary" near-misses under
// SaveRejectedMode::Boundary. Fixed at construction time (not a tunable)
// because it purely controls what we OBSERVE, not what we decide: the
// gate's own decision has already been made when we consult this.
constexpr float kBoundaryBandwidthMarginKhz = 0.30f;

// UTC ISO-8601 with millisecond precision. Matches the log timestamp format
// so a sidecar's capture_ts and a [dsp.bed] log line can be cross-correlated
// by exact string match.
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

const char* modeName(SaveRejectedMode m) {
    switch (m) {
        case SaveRejectedMode::Off:      return "off";
        case SaveRejectedMode::All:      return "all";
        case SaveRejectedMode::Sample:   return "sample";
        case SaveRejectedMode::Boundary: return "boundary";
    }
    return "off";
}

// Attribute the rejection to the specific gate clause using the event
// features we already have + the current tunable value. Mirrors the
// two-way split the detector prints at close time
// (BandEnergyDetector: sweepBatLike ? "temporal" : "sweep").
// "unknown" covers pathological cases: no events in the payload (spurious
// trigger that never opened a real event), or missing tunable metadata.
std::string classifyRejection(const SidecarPayload& payload,
                              const IDetectorStateProvider& detector) {
    if (payload.events.empty()) return "unknown";
    double minBw = 0.0;
    bool haveTunable = false;
    {
        std::vector<TunableValue> tv;
        if (detector.currentTunables(tv)) {
            for (const auto& t : tv) {
                if (t.key == "min_bandwidth_khz") {
                    minBw = t.value;
                    haveTunable = true;
                    break;
                }
            }
        }
    }
    if (!haveTunable) return "unknown";

    // Any rejected event whose bandwidth failed the sweep-shape gate
    // marks the whole clip as "sweep"-attributable; otherwise the
    // temporal repetition-rate guard was the sole downgrader.
    for (const auto& e : payload.events) {
        if (!e.gate_rejected) continue;
        if (static_cast<double>(e.bandwidth_khz) < minBw) return "sweep";
    }
    return "temporal";
}
} // namespace

Recorder::Recorder(RecorderConfig cfg,
                   const PreRollBuffer& preRoll,
                   IDetectorStateProvider& detector)
    : m_cfg(std::move(cfg)),
      m_preRoll(preRoll),
      m_detector(detector),
      m_names(m_cfg.outputDir) {}

Recorder::~Recorder() {
    stop();
}

void Recorder::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;
    m_thread = std::thread(&Recorder::loop, this);
}

void Recorder::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    if (m_thread.joinable()) m_thread.join();
    if (m_state == State::Active) {
        endRecording();
    }
}

void Recorder::loop() {
    const auto pollInterval = std::chrono::milliseconds(m_cfg.pollIntervalMs);
    const auto silence      = std::chrono::milliseconds(m_cfg.silenceMs);

    LS_INFO("recorder", "started preroll=%ums silence=%ums min=%ums max=%ums dir=%s",
            m_cfg.preRollMs, m_cfg.silenceMs,
            m_cfg.minLengthMs, m_cfg.maxLengthMs,
            m_cfg.outputDir.string().c_str());

    while (m_running.load(std::memory_order_acquire)) {
        const auto s = m_detector.snapshot();

        if (m_state == State::Idle) {
            if (s.active) {
                beginRecording(s);
            }
        } else {
            appendLiveAudio();
            // appendLiveAudio() flips m_state back to Idle if the max-length
            // cap closed the file — re-check before processing the detector
            // snapshot so we don't double-end an already-closed recording.
            if (m_state == State::Idle) {
                continue;
            }
            if (s.active) {
                m_lastActiveTime = std::chrono::steady_clock::now();
                if (s.loHz > 0.0f) m_eventLoHz = std::min(m_eventLoHz, s.loHz);
                if (s.hiHz > 0.0f) m_eventHiHz = std::max(m_eventHiHz, s.hiHz);
            } else {
                const auto idle = std::chrono::steady_clock::now() - m_lastActiveTime;
                if (idle >= silence) {
                    endRecording();
                }
            }
        }

        std::this_thread::sleep_for(pollInterval);
    }

    // On shutdown, flush any in-progress recording so we don't lose it.
    if (m_state == State::Active) {
        appendLiveAudio();
        endRecording();
    }
    LS_INFO("recorder", "stopped");
}

void Recorder::beginRecording(const DetectorStateSnapshot& s) {
    m_eventStartWall   = std::chrono::system_clock::now();
    m_eventStartSteady = std::chrono::steady_clock::now();
    m_lastActiveTime   = m_eventStartSteady;
    m_eventLoHz        = s.loHz > 0.0f ? s.loHz : 0.0f;
    m_eventHiHz        = s.hiHz > 0.0f ? s.hiHz : 0.0f;
    // Baseline the kept-events counter now. If the same value is still
    // observed at endRecording(), no bat-like event fired during the clip and
    // it gets discarded when cricketDiscard is enabled.
    m_batLikeAtStart              = s.batLikeEvents;
    m_lastCloseWasMaxLenActive    = false;

    m_currentTempPath = m_names.tempPath(m_eventStartWall);
    m_writer = std::make_unique<WavWriter>(m_currentTempPath,
                                           m_cfg.sampleRate, m_cfg.channels);

    const std::size_t preRollSamples =
        static_cast<std::size_t>(m_cfg.preRollMs) * m_cfg.sampleRate / 1000u;
    m_cursor = m_preRoll.openCursor(preRollSamples);

    // Discard any sidecar events that accumulated between recordings —
    // they belong to past WAVs (or none at all, if the detector triggered
    // briefly without crossing the recorder's gate). The detector will
    // snapshot a fresh noise floor on the FIRST event of THIS recording.
    if (m_cfg.writeSidecar) {
        SidecarPayload stale;
        m_detector.drainSidecarPayload(stale);
    }

    // Per-recording lifecycle at DEBUG so the default operational log
    // stays quiet on a busy night. The HEARTBEAT counter + the saved WAV
    // itself + its sidecar are the operator's summary; --log-level debug
    // re-enables the per-recording trace for offline diagnosis.
    LS_DEBUG("recorder", "RECORDING_OPEN file=%s preroll=%zu samples band=%.1f-%.1fkHz",
             m_currentTempPath.filename().string().c_str(), preRollSamples,
             m_eventLoHz / 1000.0f, m_eventHiHz / 1000.0f);

    m_state = State::Active;
    appendLiveAudio();
}

void Recorder::appendLiveAudio() {
    if (!m_writer) return;
    std::array<std::int16_t, kDrainChunkSamples> chunk{};

    // 0 disables the cap. Pre-compute as a frame count so the per-chunk check
    // is one comparison, not a division.
    const std::uint64_t maxFrames = (m_cfg.maxLengthMs > 0)
        ? framesForMs(m_cfg.maxLengthMs, m_cfg.sampleRate)
        : std::numeric_limits<std::uint64_t>::max();

    for (;;) {
        if (m_writer->framesWritten() >= maxFrames) {
            // A max-length forced close on a still-active event means the
            // detector hasn't closed the event yet and therefore hasn't
            // stamped the kept-events counter. Flag the close so
            // endRecording() keeps the clip instead of cricket-discarding
            // a mid-flight event.
            m_lastCloseWasMaxLenActive = m_detector.snapshot().active;
            endRecording();   // close + rename (or discard if under min)
            return;
        }
        std::size_t lost = 0;
        const std::size_t got = m_preRoll.read(m_cursor,
                                               std::span<std::int16_t>(chunk),
                                               lost);
        if (lost > 0) {
            LS_WARN("recorder", "preroll overrun: dropped %zu samples", lost);
        }
        if (got == 0) break;

        // Clip the write so we never overshoot the budget by up to a chunk.
        const std::uint64_t remaining = maxFrames - m_writer->framesWritten();
        const std::size_t   toWrite   = static_cast<std::size_t>(
            std::min<std::uint64_t>(got, remaining));
        m_writer->write(std::span<const std::int16_t>(chunk.data(), toWrite));
        if (toWrite < got) {
            m_lastCloseWasMaxLenActive = m_detector.snapshot().active;
            endRecording();
            return;
        }
    }
}

void Recorder::endRecording() {
    if (!m_writer) {
        m_state = State::Idle;
        return;
    }

    const std::uint64_t frames = m_writer->framesWritten();
    const std::uint32_t durationMs = static_cast<std::uint32_t>(
        (frames * 1000ULL) / static_cast<std::uint64_t>(m_cfg.sampleRate));

    const float loHz = m_eventLoHz > 0.0f ? m_eventLoHz : 0.0f;
    const float hiHz = m_eventHiHz > 0.0f ? m_eventHiHz : 0.0f;

    // Min-length gate: anything shorter is dropped on the floor rather than
    // finalized, so the output dir stays free of clip-sized noise events.
    if (m_cfg.minLengthMs > 0
        && frames < framesForMs(m_cfg.minLengthMs, m_cfg.sampleRate)) {
        LS_DEBUG("recorder",
                 "RECORDING_DISCARDED file=%s reason=min-length duration=%ums "
                 "(< minLengthMs=%u) band=%.1f-%.1fkHz",
                 m_currentTempPath.filename().string().c_str(),
                 durationMs, m_cfg.minLengthMs,
                 loHz / 1000.0f, hiHz / 1000.0f);
        m_writer->abort();
        m_writer.reset();
        m_state = State::Idle;
        return;
    }

    // Cricket-filter gate: discard clips whose window saw no bat-like
    // event. The detector publishes its kept-events counter through the DSP
    // snapshot; if it hasn't advanced since beginRecording() then every event
    // during this clip was rejected by the sweep gate (or no event fired at
    // all — a spurious trigger). Reading the snapshot AFTER the silence
    // timeout has elapsed means any event still open at the start of this
    // call has definitively closed and stamped the counter (guaranteed
    // because ConfigValidator enforces silenceMs >= hangover×frame_ms +
    // poll + margin). The one case where that guarantee does NOT hold is a
    // max-length forced close with the event still active — in that case
    // the triggering event has not yet stamped the counter, so we cannot
    // fairly judge it here and must keep the clip.
    if (m_cfg.cricketDiscard && !m_lastCloseWasMaxLenActive) {
        const auto s = m_detector.snapshot();
        if (s.batLikeEvents == m_batLikeAtStart) {
            // Drain the detector's staged events for this clip *first*.
            // Two roles: (a) if --save-rejected is on, we need the
            // features to classify and to write the rejected sidecar;
            // (b) drained-and-dropped keeps stale events from leaking
            // into the next recording's payload. Skip the drain only if
            // no consumer needs it (writeSidecar off AND saveRejected
            // off) to preserve the pre-feature fast path exactly.
            SidecarPayload payload;
            const bool needPayload = m_cfg.writeSidecar
                                     || m_cfg.saveRejected != SaveRejectedMode::Off;
            if (needPayload) {
                m_detector.drainSidecarPayload(payload);
            }

            // Parallel rejected sink: preserve the clip under
            // <output>/rejected/ instead of aborting. Runs entirely on
            // the recorder thread (never the audio thread) — the audio
            // path's contract is unchanged. If any step of the rejected
            // write fails, or the mode/governor declines, fall through
            // to the abort branch below so the discard behaviour is
            // preserved.
            if (m_cfg.saveRejected != SaveRejectedMode::Off
                && shouldSaveRejected(payload)
                && rejectedGovernorAllows()
                && saveRejectedClip(payload)) {
                recordRejectedWrite();
                m_writer.reset();
                m_state = State::Idle;
                return;
            }

            LS_DEBUG("recorder",
                     "RECORDING_DISCARDED file=%s reason=cricket-gate "
                     "(no bat-like event) duration=%ums band=%.1f-%.1fkHz",
                     m_currentTempPath.filename().string().c_str(),
                     durationMs, loHz / 1000.0f, hiHz / 1000.0f);
            m_writer->abort();
            m_writer.reset();
            m_state = State::Idle;
            return;
        }
    }

    const auto finalPath = m_names.finalPath(m_eventStartWall);
    m_writer->closeAndRename(finalPath);
    m_writer.reset();

    // RECORDING_SAVED trace at DEBUG; the operator-facing summary is the
    // events_kept counter in the HEARTBEAT line + the WAVs on disk.
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_eventStartSteady).count();
    LS_DEBUG("recorder", "RECORDING_SAVED file=%s frames=%llu duration=%ums elapsed=%lldms band=%.1f-%.1fkHz",
             finalPath.filename().string().c_str(),
             static_cast<unsigned long long>(frames), durationMs,
             static_cast<long long>(elapsedMs),
             loHz / 1000.0f, hiHz / 1000.0f);

    // Sidecar: per-event diagnostics + noise-floor snapshot, written
    // alongside the WAV so the offline tuning tools have everything they
    // need to reproduce the device's decision on a cold-started replay.
    // Skip if the detector exposes no diagnostics (older plugins) or the
    // operator turned it off via RecorderConfig.
    if (m_cfg.writeSidecar) {
        SidecarPayload payload;
        if (m_detector.drainSidecarPayload(payload)) {
            SidecarRecording meta;
            meta.wav_path        = finalPath.filename().string();
            meta.sample_rate     = m_cfg.sampleRate;
            meta.fft_size        = m_detector.fftSize();
            meta.hop_size        = m_detector.hopSize();
            meta.freq_lo_hz      = m_detector.freqLoHz();
            meta.freq_hi_hz      = m_detector.freqHiHz();
            meta.preroll_ms      = m_cfg.preRollMs;
            meta.silence_ms      = m_cfg.silenceMs;
            meta.algorithm       = m_detector.algorithmName();
            meta.boot_iso8601    = formatIso8601Utc(m_cfg.bootWall);
            meta.capture_iso8601 = formatIso8601Utc(m_eventStartWall);

            std::vector<TunableValue> tunables;
            m_detector.currentTunables(tunables);

            if (!writeSidecar(finalPath, meta, tunables, payload)) {
                // Sidecar write failure is non-fatal: the WAV is already
                // safely on disk. Log loud so a missing sidecar can be
                // root-caused without re-running the test.
                LS_WARN("recorder", "sidecar write failed for %s",
                        finalPath.filename().string().c_str());
            }
        }
    }

    m_state = State::Idle;
}

// --- Rejected-sink helpers -----------------------------------------------
//
// Runs on the recorder thread only. The audio thread's push into the pre-
// roll buffer is unchanged; the DSP thread's snapshot publication is
// unchanged. Adding this sink strictly extends what the recorder does on
// the discard branch — the accepted branch is untouched.

bool Recorder::shouldSaveRejected(const SidecarPayload& payload) {
    switch (m_cfg.saveRejected) {
        case SaveRejectedMode::Off:
            return false;
        case SaveRejectedMode::All:
            return true;
        case SaveRejectedMode::Sample: {
            const std::uint32_t n = m_cfg.saveRejectedSampleN > 0
                                    ? m_cfg.saveRejectedSampleN : 1u;
            std::uniform_int_distribution<std::uint32_t> d(0, n - 1);
            return d(m_rng) == 0;
        }
        case SaveRejectedMode::Boundary: {
            // Near-miss iff any rejected event's 10-dB bandwidth sits
            // within kBoundaryBandwidthMarginKhz of the sweep-shape gate's
            // primary threshold. Skips a boundary-mode write when we
            // can't query the tunable (older plugin: fall through to
            // "all", since the operator explicitly asked us to observe).
            std::vector<TunableValue> tv;
            double minBw = 0.0;
            bool have = false;
            if (m_detector.currentTunables(tv)) {
                for (const auto& t : tv) {
                    if (t.key == "min_bandwidth_khz") {
                        minBw  = t.value;
                        have   = true;
                        break;
                    }
                }
            }
            if (!have) return true;
            for (const auto& e : payload.events) {
                if (!e.gate_rejected) continue;
                const double d = std::fabs(static_cast<double>(e.bandwidth_khz) - minBw);
                if (d <= static_cast<double>(kBoundaryBandwidthMarginKhz)) {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

bool Recorder::rejectedGovernorAllows() {
    // 0 = unlimited (local offline-validation mode).
    if (m_cfg.saveRejectedMaxPerHour == 0) {
        // Still perform the low-disk check so a runaway local run doesn't
        // silently fill the boot volume.
    } else {
        const auto now       = std::chrono::steady_clock::now();
        const auto oneHourAgo = now - std::chrono::hours(1);
        while (!m_rejectedWriteTimes.empty()
               && m_rejectedWriteTimes.front() < oneHourAgo) {
            m_rejectedWriteTimes.pop_front();
        }
        if (m_rejectedWriteTimes.size() >= m_cfg.saveRejectedMaxPerHour) {
            // Log at most once per cap-hit to keep the log quiet on a
            // noisy site — a tail-of-log operator sees exactly one line
            // per cap event.
            LS_DEBUG("recorder",
                     "REJECTED_CAP hit (%u/hour) — dropping this "
                     "rejected clip",
                     m_cfg.saveRejectedMaxPerHour);
            return false;
        }
    }

    if (m_cfg.saveRejectedMinDiskMb > 0) {
        std::error_code ec;
        auto s = std::filesystem::space(m_cfg.outputDir, ec);
        if (!ec) {
            const std::uint64_t freeMb = s.available / (1024ULL * 1024ULL);
            if (freeMb < m_cfg.saveRejectedMinDiskMb) {
                if (!m_rejectedLowDiskLogged) {
                    LS_WARN("recorder",
                            "REJECTED_LOW_DISK free=%lluMB (< %uMB) — "
                            "suppressing rejected-clip writes until "
                            "disk recovers",
                            static_cast<unsigned long long>(freeMb),
                            m_cfg.saveRejectedMinDiskMb);
                    m_rejectedLowDiskLogged = true;
                }
                return false;
            }
            m_rejectedLowDiskLogged = false;
        }
    }
    return true;
}

void Recorder::recordRejectedWrite() {
    ++m_rejectedWrittenTotal;
    m_rejectedWriteTimes.push_back(std::chrono::steady_clock::now());
}

bool Recorder::saveRejectedClip(const SidecarPayload& payload) {
    const auto rejPath = m_names.rejectedPath(m_eventStartWall);

    // Rename the temp WAV into the rejected/ tree. WavWriter::closeAndRename
    // creates parent dirs and falls back to copy+remove across filesystems.
    // Failure here means the clip is lost — fall through so endRecording()
    // reverts to abort() and the operator sees the existing discard log.
    try {
        m_writer->closeAndRename(rejPath);
    } catch (...) {
        LS_WARN("recorder", "rejected-sink rename failed for %s",
                m_currentTempPath.filename().string().c_str());
        return false;
    }

    // Sidecar: reuse the accepted-clip payload; add the rejected tag
    // block so downstream tooling can trivially split accepted vs
    // rejected sidecars on the "rejected" key.
    if (m_cfg.writeSidecar) {
        SidecarRecording meta;
        meta.wav_path        = rejPath.filename().string();
        meta.sample_rate     = m_cfg.sampleRate;
        meta.fft_size        = m_detector.fftSize();
        meta.hop_size        = m_detector.hopSize();
        meta.freq_lo_hz      = m_detector.freqLoHz();
        meta.freq_hi_hz      = m_detector.freqHiHz();
        meta.preroll_ms      = m_cfg.preRollMs;
        meta.silence_ms      = m_cfg.silenceMs;
        meta.algorithm       = m_detector.algorithmName();
        meta.boot_iso8601    = formatIso8601Utc(m_cfg.bootWall);
        meta.capture_iso8601 = formatIso8601Utc(m_eventStartWall);
        meta.rejected_reason = classifyRejection(payload, m_detector);
        meta.rejected_mode   = modeName(m_cfg.saveRejected);

        std::vector<TunableValue> tunables;
        m_detector.currentTunables(tunables);
        if (!writeSidecar(rejPath, meta, tunables, payload)) {
            LS_WARN("recorder", "rejected-sink sidecar write failed for %s",
                    rejPath.filename().string().c_str());
            // WAV is already safely on disk; a missing sidecar is a
            // recoverable operational glitch, not a reason to reject the
            // clip preservation.
        }
    }

    LS_DEBUG("recorder",
             "REJECTED_SAVED file=%s reason=%s mode=%s",
             rejPath.filename().string().c_str(),
             classifyRejection(payload, m_detector).c_str(),
             modeName(m_cfg.saveRejected));
    return true;
}

} // namespace echobox::recorder
