// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Application implementation. See Application.hpp for the public contract.

#include "Application.hpp"

#include "audio/AlsaAudioSource.hpp"
#include "common/PathUtils.hpp"
#include "common/SignalHandler.hpp"
#include "dsp/TrackerRegistry.hpp"
#include "logging/Logger.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#ifndef ECHOBOX_FIRMWARE_SHA
#define ECHOBOX_FIRMWARE_SHA "unknown"
#endif

#ifndef ECHOBOX_DYNAMIC_PLUGINS
// Release builds statically link the detector; its plugin entry points are
// resolved at link time rather than via dlopen.
extern "C" {
    ISweepTracker* create_tracker();
    void           destroy_tracker(ISweepTracker*);
    const char*    get_tracker_name();
}
#endif

namespace echobox::app {

namespace {

constexpr std::size_t kCaptureChunkFrames = 4096;

// Operational-log thresholds. Deliberately generous so a healthy
// deployment never spams the log; a WARN here means the operator actually
// needs to intervene.
constexpr std::uint64_t kLowDiskMbThreshold = 200;   // 200 MB free
constexpr std::uint64_t kLowMemMbThreshold  = 32;    // 32 MB available

std::uint64_t availableDiskMb(const std::filesystem::path& p) {
    std::error_code ec;
    auto s = std::filesystem::space(p, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(s.available / (1024ULL * 1024ULL));
}

// /proc/meminfo:MemAvailable in MB. Non-Linux platforms return 0 (WARN-safe
// default: a threshold check against 0 fires but there's no /proc on macOS
// dev boxes to check anyway).
std::uint64_t availableMemMb() {
    std::ifstream in("/proc/meminfo");
    if (!in) return 0;
    std::string key;
    long value = 0;
    std::string unit;
    while (in >> key >> value >> unit) {
        if (key == "MemAvailable:") {
            // /proc/meminfo reports kB by convention.
            return static_cast<std::uint64_t>(value) / 1024ULL;
        }
    }
    return 0;
}

std::size_t preRollCapacitySamples(const Config& cfg) {
    // preroll + 500 ms safety so a disk-flush hiccup doesn't lose pre-roll data.
    const std::uint64_t ms = static_cast<std::uint64_t>(cfg.preRollMs) + 500;
    return static_cast<std::size_t>(ms * cfg.sampleRate / 1000ULL) * cfg.channels;
}

std::size_t dspRingCapacity(const Config& cfg) {
    // Sized to absorb realistic ALSA delivery bursts. The Ultramic period is
    // ~10 ms and aloop-based validation can deliver 3–5 periods back-to-back
    // when the playback side pauses or resumes; a ring covering ~85 ms of
    // audio has enough headroom to swallow that without triggering
    // spurious OVERFLOW warnings. At 384 kHz mono float32 this is ~130 kB —
    // negligible on any deployment target we care about.
    return cfg.hopSize * 128;
}

} // namespace

// Adapter: turns Recorder::IRecorderDecisionSink calls into JSONL lines on
// the shared DecisionLog. Lives inside Application because it is
// exclusively wired between Application-owned components; no reason to
// expose it to callers.
class Application::RecorderDecisionForwarder
    : public ::echobox::collection::IRecorderDecisionSink {
public:
    explicit RecorderDecisionForwarder(::echobox::collection::DecisionLog& log)
        : m_log(log) {}

    void onRecorderDecision(
        const ::echobox::collection::RecorderDecision& d) override {
        // Deliberately hand-rolled — the JSON body is a fixed schema and
        // dragging in a JSON library for six fields is not worth the
        // link-size hit on the Pi Zero 2 W.
        std::ostringstream oss;
        oss << "{\"kind\":\"decision\","
            << "\"clip_start_sample\":" << d.clip_start_sample << ","
            << "\"clip_end_sample\":"   << d.clip_end_sample   << ","
            << "\"bat_like_at_start\":" << d.bat_like_at_start << ","
            << "\"bat_like_at_end\":"   << d.bat_like_at_end   << ","
            << "\"duration_ms\":"       << d.duration_ms       << ","
            << "\"event_lo_hz\":"       << d.event_lo_hz       << ","
            << "\"event_hi_hz\":"       << d.event_hi_hz       << ","
            << "\"saved\":"             << (d.saved ? "true" : "false") << ","
            << "\"reason\":\""          << d.reason << "\""
            << "}";
        m_log.append(oss.str());
    }

private:
    ::echobox::collection::DecisionLog& m_log;
};

Application::Application(Config cfg)
    : m_cfg(std::move(cfg)),
      m_dspRing(dspRingCapacity(m_cfg)),
      m_preRoll(preRollCapacitySamples(m_cfg)) {}

Application::~Application() = default;

void Application::scanPlugins() {
#ifdef ECHOBOX_DYNAMIC_PLUGINS
    fs::path algoPath = PathUtils::getExecutableDir() / "algorithms";
    LS_INFO("app", "scanning plugins in %s", algoPath.string().c_str());
    ::TrackerRegistry::getInstance().scanPlugins(algoPath.string());
#else
    ::TrackerRegistry::getInstance().registerBuiltin(
        create_tracker, destroy_tracker, get_tracker_name);
#endif
    for (const auto& a : ::TrackerRegistry::getInstance().getAvailableAlgorithms()) {
        LS_INFO("registry", "loaded algorithm %s", a.c_str());
    }
}

std::unique_ptr<audio::IAudioSource> Application::makeAudioSource() {
    audio::AlsaAudioSource::Params p;
    p.device     = m_cfg.device;
    p.sampleRate = m_cfg.sampleRate;
    p.channels   = m_cfg.channels;
    return std::make_unique<audio::AlsaAudioSource>(p);
}

int Application::run() {
    common::SignalHandler::install();

    // Logging: on (info) by default so a field unit ships with a
    // paper trail. --log-level off remains fully silent (no file, no
    // console) for power/SD-card conservation.
    if (m_cfg.logLevel != logging::LogLevel::Off) {
        logging::LoggerConfig logCfg;
        logCfg.dir      = m_cfg.logDir;
        logCfg.minLevel = m_cfg.logLevel;
        logCfg.console  = m_cfg.consoleLog;
        logging::Logger::instance().start(logCfg);
    }

    LS_INFO("app",
            "Echobox starting sr=%d ch=%d alg=%s window=%d-%dHz "
            "cricket_filter=%s log_level=%s console=%s",
            m_cfg.sampleRate, m_cfg.channels, m_cfg.algorithm.c_str(),
            m_cfg.freqLoHz, m_cfg.freqHiHz,
            m_cfg.cricketFilter ? "on" : "off",
            logging::levelName(m_cfg.logLevel),
            m_cfg.consoleLog ? "on" : "off");

    scanPlugins();

    try {
        m_source = makeAudioSource();
        m_source->open();
    } catch (const std::exception& e) {
        LS_ERROR("audio", "MIC_OPEN_FAILED device='%s' %s",
                 m_cfg.device.c_str(), e.what());
        logging::Logger::instance().stop();
        return 2;
    }

    if (m_source->sampleRate() != m_cfg.sampleRate) {
        LS_WARN("app", "device sample rate %d differs from CLI %d; using device rate",
                m_source->sampleRate(), m_cfg.sampleRate);
        m_cfg.sampleRate = m_source->sampleRate();
    }

    dsp::DspPipelineConfig dcfg;
    dcfg.sampleRate = m_cfg.sampleRate;
    dcfg.fftSize    = m_cfg.fftSize;
    dcfg.hopSize    = m_cfg.hopSize;
    dcfg.freqLoHz   = static_cast<float>(m_cfg.freqLoHz);
    dcfg.freqHiHz   = static_cast<float>(m_cfg.freqHiHz);
    dcfg.algorithm     = m_cfg.algorithm;
    dcfg.snrThreshold  = m_cfg.snrThreshold;
    dcfg.cricketFilter = m_cfg.cricketFilter;
    m_dsp = std::make_unique<dsp::DspPipeline>(dcfg, m_dspRing);

    recorder::RecorderConfig rcfg;
    rcfg.outputDir = m_cfg.outputDir;
    rcfg.sampleRate = m_cfg.sampleRate;
    rcfg.channels   = m_cfg.channels;
    rcfg.preRollMs   = m_cfg.preRollMs;
    rcfg.silenceMs   = m_cfg.silenceMs;
    rcfg.minLengthMs = m_cfg.minLengthMs;
    rcfg.maxLengthMs = m_cfg.maxLengthMs;
    // Cricket filter: one CLI flag flips both halves. Detector-side lands
    // via DspPipelineConfig.cricketFilter → sweep_gate_enabled tunable
    // above; recorder-side is the post-hoc no-bat-like-event discard here.
    rcfg.cricketDiscard = m_cfg.cricketFilter;
    m_recorder = std::make_unique<recorder::Recorder>(rcfg, m_preRoll, *m_dsp);

    // Data-collection overlay: constructed only when enabled so a shipping
    // unit with the flag off pays zero cost. Session::start() writes the
    // pre-flight header + capacity report, spawns the governor thread, and
    // is the first artefact on the collection card.
    if (m_cfg.collection.enabled) {
        // Stream A ring: sized for ~8 s at the configured sample rate.
        // Comfortable headroom for SD-card fsync stalls without inviting
        // drops. Only allocated when the overlay is enabled AND Stream A
        // is selected — a small-card deployment that drops A still keeps
        // the sample clock and Session ready for B/C/D.
        if (m_cfg.collection.streams.streamA) {
            const std::size_t refCapacity =
                static_cast<std::size_t>(m_cfg.sampleRate) * 8;
            m_referenceRing = std::make_unique<
                ::echobox::collection::ReferenceRing>(refCapacity);
            ::echobox::collection::ContinuousWriterConfig cwCfg;
            cwCfg.outputDir   = m_cfg.collection.dir / "reference";
            cwCfg.sampleRate  = m_cfg.sampleRate;
            cwCfg.channels    = m_cfg.channels;
            m_continuousWriter = std::make_unique<
                ::echobox::collection::ContinuousWriter>(
                cwCfg, *m_referenceRing, m_sampleClock);
        }
        // Stream B preroll: 8 seconds — comfortably wider than any clip
        // window we'll ask for, and sized so a cricket-heavy burst can
        // back the writer thread up for several seconds without a job
        // aging out of the ring (a 2 s ring silently lost ~3% of events
        // during a 9.7 h field session). At 384 kHz mono int16 the ring
        // is ~6 MB — cheap on the Pi Zero 2 W's 512 MB RAM.
        // Independent of Stream A ring; the two are populated in lockstep
        // from the capture loop so the byte-identity check in §3 diffs
        // matching sample ranges.
        if (m_cfg.collection.streams.streamB) {
            const std::size_t bufSamples =
                static_cast<std::size_t>(m_cfg.sampleRate) * 8;
            m_collectionPreRoll = std::make_unique<recorder::PreRollBuffer>(bufSamples);
        }
        // Streams B and C both write to the decision-log directory tree,
        // so the log lives one level above at <collection-dir>/events.jsonl
        // (Stream C) and <collection-dir>/events/{accepted,rejected}/*
        // (Stream B WAVs).
        if (m_cfg.collection.streams.streamC) {
            m_decisionLog = std::make_unique<::echobox::collection::DecisionLog>(
                m_cfg.collection.dir / "decisions.jsonl");
        }
        m_session = std::make_unique<::echobox::collection::Session>(
            m_cfg.collection, m_sampleClock, m_cfg.collection.dir);
        ::echobox::collection::SessionMetadata meta;
        meta.firmwareSha = ECHOBOX_FIRMWARE_SHA;
        meta.algorithm   = m_cfg.algorithm;
        meta.sampleRate  = m_cfg.sampleRate;
        meta.channels    = m_cfg.channels;
        meta.micDevice   = m_cfg.device;
        meta.siteNote    = m_cfg.collection.siteNote;
        // Small JSON blob of the decision-relevant knobs, so a header alone
        // reproduces the shipping recorder's behaviour on this run.
        std::ostringstream cfgBlob;
        cfgBlob << "{"
                << "\"preroll_ms\":"     << m_cfg.preRollMs      << ","
                << "\"silence_ms\":"     << m_cfg.silenceMs      << ","
                << "\"min_length_ms\":"  << m_cfg.minLengthMs    << ","
                << "\"max_length_ms\":"  << m_cfg.maxLengthMs    << ","
                << "\"snr_threshold\":"  << m_cfg.snrThreshold   << ","
                << "\"fft_size\":"       << m_cfg.fftSize        << ","
                << "\"hop_size\":"       << m_cfg.hopSize        << ","
                << "\"freq_lo_hz\":"     << m_cfg.freqLoHz       << ","
                << "\"freq_hi_hz\":"     << m_cfg.freqHiHz       << ","
                << "\"cricket_filter\":" << (m_cfg.cricketFilter ? "true" : "false")
                << "}";
        meta.configJsonBlob = cfgBlob.str();
        if (!m_session->start(meta)) {
            LS_ERROR("collection",
                     "collection overlay failed to start; aborting so a partial "
                     "session is never mistaken for a good one");
            m_recorder.reset();
            m_source->close();
            logging::Logger::instance().stop();
            return 4;
        }
        // Wire Stream C poller + Stream B clip writer + Recorder
        // decision forwarder. All are no-ops when their respective
        // stream toggle is off.
        if (m_decisionLog) {
            if (m_collectionPreRoll) {
                ::echobox::collection::EventClipConfig ecCfg;
                ecCfg.outputDir  = m_cfg.collection.dir / "events";
                ecCfg.sampleRate = m_cfg.sampleRate;
                ecCfg.channels   = m_cfg.channels;
                ecCfg.hopSize    = m_cfg.hopSize;
                m_eventClipWriter =
                    std::make_unique<::echobox::collection::EventClipWriter>(
                        ecCfg, *m_collectionPreRoll, m_sampleClock);
            }
            ::echobox::collection::EventPollerConfig epCfg;
            epCfg.hopSize    = m_cfg.hopSize;
            epCfg.sampleRate = m_cfg.sampleRate;
            m_eventPoller = std::make_unique<::echobox::collection::EventPoller>(
                epCfg, *m_dsp, *m_decisionLog);
            if (m_eventClipWriter) m_eventPoller->setClipWriter(m_eventClipWriter.get());

            m_decisionForwarder =
                std::make_unique<RecorderDecisionForwarder>(*m_decisionLog);
            m_recorder->setDecisionSink(m_decisionForwarder.get());
        }
        // Stream A starts AFTER the session header is on disk so a crash
        // during Session::start() leaves no orphan WAV chunks with no
        // header to interpret them.
        if (m_continuousWriter) m_continuousWriter->start();
        if (m_eventClipWriter)  m_eventClipWriter->start();
        if (m_eventPoller)      m_eventPoller->start();
    }

    try {
        m_dsp->start();
    } catch (const std::exception& e) {
        LS_ERROR("app", "DSP start failed: %s", e.what());
        m_source->close();
        logging::Logger::instance().stop();
        return 3;
    }
    m_recorder->start();

    // Startup disk/memory sanity: surface a WARN if the unit was deployed
    // with an SD card that's already nearly full, before we start writing
    // WAVs to it.
    {
        const auto disk = availableDiskMb(m_cfg.outputDir);
        const auto mem  = availableMemMb();
        if (disk > 0 && disk < kLowDiskMbThreshold) {
            LS_WARN("app", "LOW_DISK free=%lluMB at startup (< %lluMB)",
                    static_cast<unsigned long long>(disk),
                    static_cast<unsigned long long>(kLowDiskMbThreshold));
        }
        if (mem > 0 && mem < kLowMemMbThreshold) {
            LS_WARN("app", "LOW_MEM avail=%lluMB at startup (< %lluMB)",
                    static_cast<unsigned long long>(mem),
                    static_cast<unsigned long long>(kLowMemMbThreshold));
        }
    }

    // One-shot READY line: the operator's grep target for "did the unit
    // finish coming up?". Includes the fields most likely to be wrong on
    // first deploy (device, output dir, cricket filter state).
    LS_INFO("app", "READY listening='%s' output='%s' cricket_filter=%s",
            m_cfg.device.c_str(), m_cfg.outputDir.string().c_str(),
            m_cfg.cricketFilter ? "on" : "off");

    m_running.store(true, std::memory_order_release);
    m_captureThread = std::thread(&Application::captureLoop, this);

    std::printf("Echobox is running — listening on '%s', saving recordings to '%s'.\n"
                "Press Ctrl+C to stop.\n",
                m_cfg.device.c_str(), m_cfg.outputDir.string().c_str());
    std::fflush(stdout);

    // Main loop: wait for a shutdown signal. While we're at it, emit a
    // heartbeat every heartbeatSec so the field operator can see the unit
    // is alive without triggering any events. Counters here are cheap
    // atomic loads plus a lightweight std::filesystem::space() call.
    const auto pollInterval = std::chrono::milliseconds(100);
    const auto heartbeatInterval = (m_cfg.heartbeatSec > 0)
        ? std::chrono::seconds(m_cfg.heartbeatSec)
        : std::chrono::seconds(0);
    const auto startedAt = std::chrono::steady_clock::now();
    auto lastHeartbeat  = startedAt;
    while (!common::SignalHandler::shouldExit()) {
        // Governor coordinates through the same clean-shutdown path as
        // SIGINT: no separate "governor-tore-this-down" code branch.
        if (m_session && m_session->stopRequested()) {
            LS_INFO("app", "shutdown requested (governor: %s)",
                    m_session->stopReason().c_str());
            common::SignalHandler::requestExit();
            break;
        }
        std::this_thread::sleep_for(pollInterval);
        if (heartbeatInterval.count() > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastHeartbeat >= heartbeatInterval) {
                lastHeartbeat = now;
                const auto uptimeSec =
                    std::chrono::duration_cast<std::chrono::seconds>(now - startedAt).count();
                const auto s     = m_dsp->snapshot();
                const auto disk  = availableDiskMb(m_cfg.outputDir);
                const auto mem   = availableMemMb();
                LS_INFO("app",
                        "HEARTBEAT uptime=%llds events_kept=%llu "
                        "disk_free_mb=%llu mem_avail_mb=%llu dsp_dropped=%llu",
                        static_cast<long long>(uptimeSec),
                        static_cast<unsigned long long>(s.batLikeEvents),
                        static_cast<unsigned long long>(disk),
                        static_cast<unsigned long long>(mem),
                        static_cast<unsigned long long>(m_dspDroppedTotal));
                if (disk > 0 && disk < kLowDiskMbThreshold) {
                    LS_WARN("app", "LOW_DISK free=%lluMB (< %lluMB threshold)",
                            static_cast<unsigned long long>(disk),
                            static_cast<unsigned long long>(kLowDiskMbThreshold));
                }
                if (mem > 0 && mem < kLowMemMbThreshold) {
                    LS_WARN("app", "LOW_MEM avail=%lluMB (< %lluMB threshold)",
                            static_cast<unsigned long long>(mem),
                            static_cast<unsigned long long>(kLowMemMbThreshold));
                }
            }
        }
    }
    LS_INFO("app", "shutdown requested (signal)");

    // Stop in reverse order of start.
    m_running.store(false, std::memory_order_release);
    if (m_captureThread.joinable()) m_captureThread.join();

    m_recorder->stop();
    m_dsp->stop();
    m_source->close();
    // Collection teardown order: poller first (stops feeding jobs to the
    // clip writer), clip writer next (drain in-flight jobs), continuous
    // writer, then session end-marker. Every collection artefact is on
    // disk before SESSION_END lands.
    if (m_eventPoller)      m_eventPoller->stop();
    if (m_eventClipWriter)  m_eventClipWriter->stop();
    if (m_continuousWriter) m_continuousWriter->stop();
    if (m_session)          m_session->stop();

    LS_INFO("app", "stopped");
    logging::Logger::instance().stop();
    std::puts("Echobox stopped.");
    return 0;
}

void Application::captureLoop() {
    std::array<std::int16_t, kCaptureChunkFrames> intBuf{};
    std::array<float, kCaptureChunkFrames>        floatBuf{};
    constexpr float kInvScale = 1.0f / 32768.0f;

    // Startup grace: for the first two seconds after captureLoop starts,
    // absorb DSP-ring drops silently. ALSA (and especially aloop-based
    // validation) often delivers a burst of periods at first read while the
    // driver flushes its pre-buffered state; those samples aren't
    // scientifically meaningful and the DSP thread catches up within a
    // hop or two. Warning about them just trains operators to ignore the
    // warning, which is worse than not emitting it.
    const auto captureStartedAt = std::chrono::steady_clock::now();
    constexpr auto kStartupGrace = std::chrono::seconds(2);

    while (m_running.load(std::memory_order_acquire)) {
        int got = m_source->read(std::span<std::int16_t>(intBuf));
        if (got < 0) {
            LS_ERROR("audio",
                     "MIC_DISCONNECTED — capture failed, shutting down");
            common::SignalHandler::requestExit();
            break;
        }
        if (got == 0) continue;

        const std::size_t n = static_cast<std::size_t>(got);

        // 1. Feed the recorder's pre-roll buffer (raw int16, mono).
        m_preRoll.write(std::span<const std::int16_t>(intBuf.data(), n));

        // 1b. Advance the global collection sample clock. Gated on the
        //     enabled flag so a shipping unit with the overlay off pays
        //     literally zero cost here (no atomic RMW, no memory barrier
        //     beyond what preRoll.write already emits). Kill-switch
        //     discipline: byte-identical to R2v3 when disabled.
        if (m_cfg.collection.enabled) {
            m_sampleClock.advance(static_cast<std::uint64_t>(n));
        }

        // 1c. Stream A tap: push raw int16 samples into the reference
        //     ring for the continuous writer thread. Wait-free; drops are
        //     counted on the ring and surfaced via a rate-limited warning
        //     (matches the DSP-drop discipline in the same loop). The
        //     plan (§2.2 A, §3) treats any drop as a session-invalidating
        //     event that the offline integrity checks must catch.
        if (m_referenceRing) {
            const std::size_t refDropped = m_referenceRing->pushBatch(
                std::span<const std::int16_t>(intBuf.data(), n));
            if (refDropped > 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now - m_lastRefDropWarnAt >= std::chrono::seconds(1)) {
                    LS_WARN("collection",
                            "STREAM_A_OVERFLOW dropped=%zu total=%llu — "
                            "writer is not keeping up; §3 gap check will fail",
                            refDropped,
                            static_cast<unsigned long long>(
                                m_referenceRing->dropped()));
                    m_lastRefDropWarnAt = now;
                }
            }
        }

        // 1d. Stream B preroll tap: mirror the same samples into the
        //     collection preroll ring so EventClipWriter can extract
        //     per-event windows on cursor-open. Same wait-free discipline;
        //     the ring is overwriting so a lagging clip writer costs at
        //     worst old-window data (logged as "aged out").
        if (m_collectionPreRoll) {
            m_collectionPreRoll->write(
                std::span<const std::int16_t>(intBuf.data(), n));
        }

        // 2. Convert + push into the DSP ring, best-effort. The audio thread
        //    MUST NOT block on a downstream consumer: a stalled DSP would
        //    back-pressure into ALSA and cause an xrun. Count overflow and
        //    surface it via a rate-limited warning instead.
        for (std::size_t i = 0; i < n; ++i) {
            floatBuf[i] = static_cast<float>(intBuf[i]) * kInvScale;
        }
        std::size_t dropped = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!m_dspRing.push(floatBuf[i])) ++dropped;
        }
        if (dropped > 0) {
            m_dspDroppedTotal += dropped;
            const auto now = std::chrono::steady_clock::now();
            const bool inGrace = (now - captureStartedAt) < kStartupGrace;
            if (!inGrace
                && now - m_lastDropWarnAt >= std::chrono::seconds(1)) {
                // Grep-consistent OVERFLOW anchor to match the RECORDING_/
                // MIC_/HEARTBEAT style used elsewhere.
                LS_WARN("dsp", "OVERFLOW dropped=%zu total=%llu",
                        dropped,
                        static_cast<unsigned long long>(m_dspDroppedTotal));
                m_lastDropWarnAt = now;
            }
        }

        // 3. Wake the DSP thread once per chunk. notify_one on an unwaited
        //    CV is a single atomic; cheap to call even when the DSP isn't
        //    parked.
        m_dsp->notifyInput();
    }
}

} // namespace echobox::app
