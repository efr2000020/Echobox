// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// echobox-replay — feed a directory of WAV files through the SAME
/// DspPipeline + Recorder the shipping binary uses, so a corpus of field
/// recordings can produce a real accepted/ + rejected/ layout for offline
/// analysis. Build-gated (ECHOBOX_BUILD_REPLAY=ON); never in the shipping
/// binary.
///
/// Design note: we deliberately do NOT go through Application. Application
/// is the field runtime — it owns SIGINT polling, heartbeats, ALSA-only
/// source construction, and startup printf. Replay reuses the two things
/// that matter (DspPipeline + Recorder) and keeps its own capture loop, so
/// (a) src/app/ compiles bit-identically with or without replay built, and
/// (b) replay-only concerns (drop-free backpressure, EOF-not-fatal, x86
/// caveat) don't pollute the field code path.

#include "WavFileAudioSource.hpp"
#include "SessionHeader.hpp"
#include "RunManifest.hpp"
#include "VirtualClock.hpp"

#include "app/Config.hpp"
#include "app/ConfigValidator.hpp"
#include "common/LockFreeRingBuffer.hpp"
#include "common/PathUtils.hpp"
#include "dsp/DspPipeline.hpp"
#include "dsp/TrackerRegistry.hpp"
#include "logging/Logger.hpp"
#include "recorder/PreRollBuffer.hpp"
#include "recorder/Recorder.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#ifndef ECHOBOX_DYNAMIC_PLUGINS
extern "C" {
    ISweepTracker* create_tracker();
    void           destroy_tracker(ISweepTracker*);
    const char*    get_tracker_name();
}
#endif

namespace fs = std::filesystem;
using echobox::app::Config;

namespace {

constexpr std::size_t kCaptureChunkFrames = 4096;

/// Wall instant that replay's virtual time zero maps to.
///
/// Deliberately a fixed constant (the Unix epoch) rather than "now": the
/// recorder stamps this into every WAV filename and into the sidecars'
/// capture_iso8601, so anything derived from the launch time would make
/// two replays of the same corpus differ on disk. With this epoch a clip
/// filename reads as the offset into the replayed stream — 19700101_
/// 000321450.wav is the clip that starts 3 m 21.450 s in — which is
/// also a useful signal that the timestamp describes replay time and not
/// when the audio was captured. Nothing downstream parses it as a date:
/// tools/session_screen/replay.py recovers a clip's true source time from
/// the sidecar's device.frames_processed counter, not from the filename.
constexpr std::chrono::system_clock::time_point kVirtualWallEpoch{};

void printHelp() {
    std::puts(
        "echobox-replay — offline validation tool (NOT for field use).\n"
        "\n"
        "Feeds a directory of WAV recordings through the same DspPipeline +\n"
        "Recorder the shipping Echobox binary uses, producing accepted/ and\n"
        "(with --save-rejected) rejected/ dirs under --output.\n"
        "\n"
        "Usage:\n"
        "  echobox-replay --input <dir-or-file> --output <dir> [options]\n"
        "\n"
        "Required:\n"
        "  --input <path>            Directory (recursively scanned) or single .wav file\n"
        "  --output <path>           Root output dir (accepted/ + rejected/ appear here)\n"
        "\n"
        "Detection & recorder (defaults match the shipped binary):\n"
        "  --algorithm <name>        (default: BandEnergyDetector)\n"
        "  --fft-size <int>          (default: 4096)\n"
        "  --hop-size <int>          (default: 512)\n"
        "  --freq-lo-hz <int>        (default: 20000)\n"
        "  --freq-hi-hz <int>        (default: 192000)\n"
        "  --snr-threshold <float>   (default: 12.0)\n"
        "  --sample-rate <hz>        Expected rate of every file (default: 384000)\n"
        "  --preroll-ms <int>        (default: 50)\n"
        "  --silence-ms <int>        (default: 50)\n"
        "  --min-length-ms <int>     (default: 0)\n"
        "  --max-length-ms <int>     (default: 200; 0 = no cap)\n"
        "  --cricket-filter on|off   (default: on) — required 'on' to get anything rejected\n"
        "\n"
        "Rejected-capture sink:\n"
        "  --save-rejected off|all|sample|boundary   (default: off)\n"
        "  --save-rejected-sample-n <int>            (default: 500)\n"
        "  --save-rejected-max-per-hour <int>        (default: 0 = unlimited for replay)\n"
        "\n"
        "\n"
        "Diagnostic tracker-tunable override (repeatable):\n"
        "  --tunable KEY=VALUE       Forwarded to the loaded plugin via\n"
        "                            setTunable() after dsp.start(). Used to\n"
        "                            A/B-test a pre-flip default against the\n"
        "                            new one without a rebuild. Example:\n"
        "                            --tunable rep_guard_enabled=1 reproduces\n"
        "                            the pre-0.3.0-rc1 temporal-veto behaviour.\n"
        "\n"
        "  -h, --help                Show this help and exit\n"
        "\n"
        "Config precedence: if a SESSION_HEADER.json is found in --input (or a\n"
        "parent dir), its capture config becomes the baseline; CLI flags above\n"
        "override. If no header is found, defaults match the shipped binary.\n"
        "\n"
        "Provenance: every run writes <output>/replay_manifest.json with the\n"
        "platform (x86-replay vs ARM device), build type, effective config, and\n"
        "the source of that config. Downstream scoring should read it.\n"
        "\n"
        "Caveat: this tool runs on x86 (Release build adds -ffast-math); the\n"
        "shipping binary runs on ARM. Feature values are a tuning proxy, not\n"
        "device-exact.\n");
}

/// Very small standalone CLI parser (subset of src/app/CliParser). The
/// shipping parser is deliberately not reused: it enforces flags that
/// don't apply to replay (--device, --log-dir, --heartbeat-sec, etc.)
/// and rejects our --input/--output additions. Keeping this here also
/// makes it obvious that touching src/app/ is unnecessary for replay.
struct ReplayCli {
    fs::path input;
    fs::path output;
    Config   cfg;
    /// Diagnostic tracker-tunable overrides. Repeatable ``--tunable
    /// KEY=VAL`` on the CLI; each pair is forwarded to the loaded
    /// plugin via ``DspPipeline::setTrackerTunable`` right after
    /// ``dsp.start()``. Used to reproduce pre-flip behaviour when a
    /// shipping default was changed (e.g. veto-recovery byte-identity
    /// check). Not for shipping — Application.cpp doesn't use them.
    std::vector<std::pair<std::string, double>> trackerTunables;
};

bool parseArg(const std::string& v, std::uint32_t& out) {
    try {
        size_t idx = 0;
        long x = std::stol(v, &idx);
        if (idx != v.size() || x < 0) return false;
        out = static_cast<std::uint32_t>(x);
        return true;
    } catch (...) { return false; }
}
bool parseArg(const std::string& v, int& out) {
    try {
        size_t idx = 0;
        long x = std::stol(v, &idx);
        if (idx != v.size()) return false;
        out = static_cast<int>(x);
        return true;
    } catch (...) { return false; }
}
bool parseArg(const std::string& v, std::size_t& out) {
    try {
        size_t idx = 0;
        long long x = std::stoll(v, &idx);
        if (idx != v.size() || x < 0) return false;
        out = static_cast<std::size_t>(x);
        return true;
    } catch (...) { return false; }
}
bool parseArg(const std::string& v, float& out) {
    try {
        size_t idx = 0;
        float x = std::stof(v, &idx);
        if (idx != v.size()) return false;
        out = x;
        return true;
    } catch (...) { return false; }
}

/// Pre-parse helper: extract the value of a single named flag from argv
/// without validating anything else. Used to look up @c --input BEFORE the
/// full CLI parse, so we can load a session header for its parent dir and
/// use the header's config as the baseline that CLI flags then override.
/// Returns an empty string if the flag is absent.
std::string preScanFlag(int argc, char** argv, const char* flag) {
    const std::string_view f(flag);
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == f) return std::string(argv[i + 1]);
    }
    return {};
}

int parseCli(int argc, char** argv, ReplayCli& out) {
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) return nullptr;
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "-h" || a == "--help") { printHelp(); std::exit(0); }
        auto req = [&](const char* name) -> std::string {
            const char* v = need(i);
            if (!v) { std::fprintf(stderr, "error: missing value for %s\n", name); std::exit(2); }
            return std::string(v);
        };

        if      (a == "--input")           out.input  = req("--input");
        else if (a == "--output")          out.output = req("--output");
        else if (a == "--algorithm")       out.cfg.algorithm = req("--algorithm");
        else if (a == "--sample-rate") {
            auto s = req("--sample-rate");
            if (!parseArg(s, out.cfg.sampleRate) || out.cfg.sampleRate <= 0) {
                std::fprintf(stderr, "error: invalid --sample-rate\n"); return 2;
            }
        }
        else if (a == "--fft-size") {
            auto s = req("--fft-size");
            if (!parseArg(s, out.cfg.fftSize) || out.cfg.fftSize == 0) {
                std::fprintf(stderr, "error: invalid --fft-size\n"); return 2;
            }
        }
        else if (a == "--hop-size") {
            auto s = req("--hop-size");
            if (!parseArg(s, out.cfg.hopSize) || out.cfg.hopSize == 0) {
                std::fprintf(stderr, "error: invalid --hop-size\n"); return 2;
            }
        }
        else if (a == "--freq-lo-hz") {
            auto s = req("--freq-lo-hz");
            if (!parseArg(s, out.cfg.freqLoHz) || out.cfg.freqLoHz < 0) {
                std::fprintf(stderr, "error: invalid --freq-lo-hz\n"); return 2;
            }
        }
        else if (a == "--freq-hi-hz") {
            auto s = req("--freq-hi-hz");
            if (!parseArg(s, out.cfg.freqHiHz) || out.cfg.freqHiHz < 0) {
                std::fprintf(stderr, "error: invalid --freq-hi-hz\n"); return 2;
            }
        }
        else if (a == "--snr-threshold") {
            auto s = req("--snr-threshold");
            if (!parseArg(s, out.cfg.snrThreshold) || !(out.cfg.snrThreshold > 0.0f)) {
                std::fprintf(stderr, "error: invalid --snr-threshold\n"); return 2;
            }
        }
        else if (a == "--preroll-ms") {
            auto s = req("--preroll-ms");
            if (!parseArg(s, out.cfg.preRollMs)) {
                std::fprintf(stderr, "error: invalid --preroll-ms\n"); return 2;
            }
        }
        else if (a == "--silence-ms") {
            auto s = req("--silence-ms");
            if (!parseArg(s, out.cfg.silenceMs)) {
                std::fprintf(stderr, "error: invalid --silence-ms\n"); return 2;
            }
        }
        else if (a == "--min-length-ms") {
            auto s = req("--min-length-ms");
            if (!parseArg(s, out.cfg.minLengthMs)) {
                std::fprintf(stderr, "error: invalid --min-length-ms\n"); return 2;
            }
        }
        else if (a == "--max-length-ms") {
            auto s = req("--max-length-ms");
            if (!parseArg(s, out.cfg.maxLengthMs)) {
                std::fprintf(stderr, "error: invalid --max-length-ms\n"); return 2;
            }
        }
        else if (a == "--cricket-filter") {
            auto s = req("--cricket-filter");
            if      (s == "on"  || s == "1") out.cfg.cricketFilter = true;
            else if (s == "off" || s == "0") out.cfg.cricketFilter = false;
            else { std::fprintf(stderr, "error: --cricket-filter must be on|off\n"); return 2; }
        }
        else if (a == "--save-rejected") {
            auto s = req("--save-rejected");
            if      (s == "off")      out.cfg.saveRejected = Config::SaveRejectedMode::Off;
            else if (s == "all")      out.cfg.saveRejected = Config::SaveRejectedMode::All;
            else if (s == "sample")   out.cfg.saveRejected = Config::SaveRejectedMode::Sample;
            else if (s == "boundary") out.cfg.saveRejected = Config::SaveRejectedMode::Boundary;
            else { std::fprintf(stderr, "error: --save-rejected must be off|all|sample|boundary\n"); return 2; }
        }
        else if (a == "--save-rejected-sample-n") {
            auto s = req("--save-rejected-sample-n");
            if (!parseArg(s, out.cfg.saveRejectedSampleN) || out.cfg.saveRejectedSampleN == 0) {
                std::fprintf(stderr, "error: invalid --save-rejected-sample-n\n"); return 2;
            }
        }
        else if (a == "--save-rejected-max-per-hour") {
            auto s = req("--save-rejected-max-per-hour");
            if (!parseArg(s, out.cfg.saveRejectedMaxPerHour)) {
                std::fprintf(stderr, "error: invalid --save-rejected-max-per-hour\n"); return 2;
            }
        }
        else if (a == "--tunable") {
            // KEY=VALUE. Split on the first '='. VALUE is parsed as
            // double so booleans (0/1), ints, and floats all round-trip
            // through the ISweepTracker::setTunable double signature.
            auto kv = req("--tunable");
            const auto eq = kv.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == kv.size()) {
                std::fprintf(stderr, "error: --tunable expects KEY=VALUE, got '%s'\n",
                             kv.c_str());
                return 2;
            }
            std::string key = kv.substr(0, eq);
            std::string val = kv.substr(eq + 1);
            try {
                double d = std::stod(val);
                out.trackerTunables.emplace_back(std::move(key), d);
            } catch (...) {
                std::fprintf(stderr, "error: --tunable value '%s' not a number\n",
                             val.c_str());
                return 2;
            }
        }
        else {
            std::fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            printHelp();
            return 2;
        }
    }

    if (out.input.empty() || out.output.empty()) {
        std::fprintf(stderr, "error: --input and --output are required\n\n");
        printHelp();
        return 2;
    }
    out.cfg.outputDir = out.output;
    return 0;
}

std::size_t dspRingCapacity(const Config& cfg) {
    // Same shape as Application::dspRingCapacity — the ring is a backpressure
    // reservoir and the replay driver waits on the push side, so a modest
    // capacity is fine.
    return cfg.hopSize * 128;
}

std::size_t preRollCapacitySamples(const Config& cfg) {
    const std::uint64_t ms = static_cast<std::uint64_t>(cfg.preRollMs) + 500;
    return static_cast<std::size_t>(ms * cfg.sampleRate / 1000ULL) * cfg.channels;
}

/**
 * @brief Feeds the pipeline one recorder poll interval of audio at a time,
 *        handing the recorder thread a turn after each one.
 *
 * This is the driver half of the protocol in VirtualClock.hpp. Each tick:
 *
 *  1. wait for the recorder to finish its previous poll and park;
 *  2. push exactly one poll interval of audio into the pre-roll buffer and
 *     the DSP ring;
 *  3. wait for the DSP thread to consume all of it;
 *  4. advance virtual time by one poll interval, which wakes the recorder
 *     for exactly one poll.
 *
 * Steps 1 and 3 are what turn "faster than real time" into "faster than
 * real time *and* identical to real time". Without 3 the recorder can poll
 * a detector snapshot that lags the audio by an unpredictable number of
 * hops; without 1 the feeder can write pre-roll samples underneath a poll
 * that is already in flight.
 *
 * Nothing here paces against wall time — the run goes exactly as fast as
 * the host can do the FFTs.
 *
 * ### Why step 3 is exact and not merely "close enough"
 *
 * DspPipeline::loop pops one sample at a time and runs the FFT + tracker +
 * publish inline, immediately after the pop that completes a hop. So the
 * moment the ring goes empty having been fed @c F samples, every hop that
 * ended at sample index @c F-2 or earlier has definitely been published —
 * popping sample @c F-1 is ordered after that work. The one ambiguous case
 * is a hop ending exactly at @c F-1: it may or may not have been published
 * yet, and *which* varies run to run.
 *
 * So the feeder simply never ends a tick on a hop boundary: if the tick's
 * frame target is a multiple of @c hopSize it feeds one extra sample. Then
 * "ring empty" always means "published exactly @c floor(F/hop) hops", which
 * is exactly what a real device would have published at that instant —
 * no more (the samples do not exist yet) and no fewer. The leftover case is
 * the end of the corpus, where the total frame count is whatever it is;
 * that is handled by @c flushFinalHop().
 */
class LockstepFeeder {
public:
    LockstepFeeder(echobox::replay::WavFileAudioSource& source,
                   LockFreeRingBuffer<float>& ring,
                   echobox::recorder::PreRollBuffer& preRoll,
                   echobox::dsp::DspPipeline& dsp,
                   echobox::replay::VirtualClock& clock,
                   int sampleRate, std::size_t hopSize,
                   std::uint32_t pollIntervalMs)
        : m_source(source), m_ring(ring), m_preRoll(preRoll), m_dsp(dsp),
          m_clock(clock),
          m_sampleRate(static_cast<std::uint64_t>(sampleRate)),
          m_hop(hopSize ? hopSize : 1),
          m_pollMs(pollIntervalMs ? pollIntervalMs : 1) {
        // One tick's worth, plus the hop-alignment sample. Reserved once so
        // the per-tick path never allocates.
        m_tick.reserve(static_cast<std::size_t>(
            m_sampleRate * m_pollMs / 1000ULL) + 2u);
    }

    /// Feed the whole corpus. Returns the number of frames fed.
    std::uint64_t run() {
        std::uint64_t fed = 0;
        for (;;) {
            m_clock.waitUntilRecorderIdle();

            ++m_tick_index;
            std::uint64_t target = framesAtTick(m_tick_index);
            // Never end a tick on a hop boundary — see the class comment.
            if (target % m_hop == 0) ++target;

            const bool more = feedUpTo(target, fed);
            waitDspCaughtUp();

            if (!more) {
                flushFinalHop(fed);
                m_clock.advanceTo(nsAtTick(m_tick_index));
                return fed;
            }
            m_clock.advanceTo(nsAtTick(m_tick_index));
        }
    }

    /**
     * @brief Let virtual time run on for @p ms with no further audio.
     *
     * The deterministic replacement for the old @c sleep_for(drainMs): it
     * gives the recorder exactly the same number of polls every run, so an
     * in-progress WAV closes on its silence timeout at the same virtual
     * instant rather than "whenever the sleep happened to expire".
     */
    void drain(std::uint32_t ms) {
        const std::uint64_t ticks = (ms + m_pollMs - 1) / m_pollMs;
        for (std::uint64_t i = 0; i < ticks; ++i) {
            m_clock.waitUntilRecorderIdle();
            m_clock.advanceTo(nsAtTick(++m_tick_index));
        }
    }

private:
    /// Total frames a real device would have captured by the end of tick
    /// @p k. Integer maths from an absolute tick index, so the audio and
    /// the clock cannot drift apart over a ten-hour corpus.
    std::uint64_t framesAtTick(std::uint64_t k) const {
        return k * m_sampleRate * m_pollMs / 1000ULL;
    }

    std::chrono::nanoseconds nsAtTick(std::uint64_t k) const {
        return std::chrono::nanoseconds(
            static_cast<std::chrono::nanoseconds::rep>(k * m_pollMs * 1'000'000ULL));
    }

    /// Pull one sample from the source, crossing file boundaries. Returns
    /// false at end of corpus.
    bool nextSample(std::int16_t& out) {
        while (m_stagePos == m_stageLen) {
            const int got = m_source.read(std::span<std::int16_t>(m_stage));
            if (got < 0) return false;   // never happens today; IAudioSource
                                         // documents it, so honour it.
            if (got == 0) {
                if (m_source.finished()) return false;
                continue;                // between-file rollover
            }
            m_stageLen = static_cast<std::size_t>(got);
            m_stagePos = 0;
        }
        out = m_stage[m_stagePos++];
        return true;
    }

    /// Feed audio until @p fed reaches @p target. Returns false at EOF.
    bool feedUpTo(std::uint64_t target, std::uint64_t& fed) {
        m_tick.clear();
        bool more = true;
        while (fed + m_tick.size() < target) {
            std::int16_t s;
            if (!nextSample(s)) { more = false; break; }
            m_tick.push_back(s);
        }
        if (!m_tick.empty()) {
            // Pre-roll first: the recorder is parked, so ordering within a
            // tick is invisible to it, but keeping the same order the audio
            // thread uses on the device costs nothing.
            m_preRoll.write(std::span<const std::int16_t>(m_tick.data(),
                                                          m_tick.size()));
            for (const std::int16_t v : m_tick) {
                pushSample(static_cast<float>(v) * kInvScale);
            }
            fed += m_tick.size();
        }
        return more;
    }

    /// Drop-free push: back off until the DSP thread makes room.
    void pushSample(float v) {
        while (!m_ring.push(v)) {
            m_dsp.notifyInput();
            std::this_thread::yield();
        }
    }

    /// Block until the DSP thread has consumed every sample pushed so far.
    void waitDspCaughtUp() {
        m_dsp.notifyInput();
        std::size_t spins = 0;
        while (m_ring.available_read() != 0) {
            // available_read() is exact when read from the producer side
            // with no push in flight: it compares our own head against the
            // consumer's published tail.
            if ((++spins & 0xFFu) == 0) m_dsp.notifyInput();
            std::this_thread::yield();
        }
    }

    /**
     * @brief Resolve the one hop the ring-empty rule cannot cover.
     *
     * At the end of the corpus the total frame count is whatever the input
     * happens to be, so it may land exactly on a hop boundary — the case
     * the per-tick alignment nudge normally avoids. Push a single sample
     * the tracker can never see (the hop accumulator restarts at zero after
     * a completed hop, so one sample cannot complete another) and wait for
     * the DSP to pop it; that pop is ordered after the publish we are
     * waiting for. Not written to the pre-roll, so no audio is altered.
     */
    void flushFinalHop(std::uint64_t fed) {
        if (fed == 0 || fed % m_hop != 0) return;
        pushSample(0.0f);
        waitDspCaughtUp();
    }

    static constexpr float kInvScale = 1.0f / 32768.0f;

    echobox::replay::WavFileAudioSource& m_source;
    LockFreeRingBuffer<float>&           m_ring;
    echobox::recorder::PreRollBuffer&    m_preRoll;
    echobox::dsp::DspPipeline&           m_dsp;
    echobox::replay::VirtualClock&       m_clock;

    const std::uint64_t m_sampleRate;
    const std::size_t   m_hop;
    const std::uint64_t m_pollMs;

    std::array<std::int16_t, kCaptureChunkFrames> m_stage{};
    std::size_t m_stageLen{0};
    std::size_t m_stagePos{0};

    std::vector<std::int16_t> m_tick;
    std::uint64_t             m_tick_index{0};
};

void scanPlugins() {
#ifdef ECHOBOX_DYNAMIC_PLUGINS
    const auto algoPath = PathUtils::getExecutableDir() / "algorithms";
    ::TrackerRegistry::getInstance().scanPlugins(algoPath.string());
#else
    ::TrackerRegistry::getInstance().registerBuiltin(
        create_tracker, destroy_tracker, get_tracker_name);
#endif
}

} // namespace

int main(int argc, char** argv) {
    ReplayCli cli;

    // 1. Replay-specific defaults (before any config source is applied):
    //    --save-rejected-max-per-hour = 0 (unlimited — replay is exhaustive
    //    corpus scoring; shipped 200/h would silently drop most of a run),
    //    logger silent unless the user opts in via --log-level.
    cli.cfg.saveRejectedMaxPerHour = 0;
    cli.cfg.logLevel   = echobox::logging::LogLevel::Off;
    cli.cfg.consoleLog = false;

    // 2. Session-header defaults. Pre-scan argv for --input, walk up from
    //    it to find a SESSION_HEADER.json, and use its capture config as
    //    the baseline. This closes the "user runs replay with shipping
    //    defaults on a corpus recorded with different settings" hole. If
    //    no header is found or parse fails, we fall back to shipping
    //    defaults (Config's ctor) and note that in the manifest.
    echobox::replay::SessionHeaderConfig sessionHdr;
    bool haveSessionHeader = false;
    fs::path preInput = preScanFlag(argc, argv, "--input");
    fs::path headerPath;
    if (!preInput.empty()) {
        headerPath = echobox::replay::findSessionHeader(preInput);
        if (!headerPath.empty()) {
            try {
                sessionHdr = echobox::replay::readSessionHeader(headerPath);
                const std::size_t n = echobox::replay::applySessionHeader(sessionHdr, cli.cfg);
                std::printf("Loaded %zu field(s) from session header: %s\n",
                            n, headerPath.string().c_str());
                haveSessionHeader = true;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "warning: session header at '%s' failed to parse: %s\n"
                                     "         falling back to shipping defaults\n",
                             headerPath.string().c_str(), e.what());
            }
        } else {
            std::printf("No SESSION_HEADER.json found near input; using shipping defaults.\n");
        }
    }

    // 3. CLI flags — highest precedence, override everything above.
    if (int rc = parseCli(argc, argv, cli); rc != 0) return rc;

    // Enumerate up front so a bad --input fails before we spin the pipeline.
    std::vector<fs::path> files;
    try {
        files = echobox::replay::enumerateWavFiles(cli.input);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }

    // Cross-flag invariants (freq-lo < freq-hi, max-length budget, etc.)
    // are enforced by the shipped validator; reuse it verbatim so replay
    // rejects the same illegal configs the field binary would.
    const auto configErrors = echobox::app::validateConfig(cli.cfg);
    if (!configErrors.empty()) {
        for (const auto& msg : configErrors) std::fprintf(stderr, "error: %s\n", msg.c_str());
        return 2;
    }

    // Honesty caveat, per the plan — printed unconditionally so a reader of
    // any generated results/log can't miss it. The exact numerics footprint
    // (fast-math on/off, Debug/Release) is stamped into replay_manifest.json
    // for downstream scoring; the message here is the operator-visible
    // headline. Print the manifest path so the reader knows where to look.
#ifdef __FAST_MATH__
    std::puts("echobox-replay: x86 build with -ffast-math; the device is ARM.");
#else
    std::puts("echobox-replay: x86 build (no -ffast-math); the device is ARM.");
#endif
    std::puts("                Feature values are a tuning proxy, not device-exact.");
    std::puts("                Full provenance in <output>/replay_manifest.json.");

    if (cli.cfg.saveRejected != Config::SaveRejectedMode::Off && !cli.cfg.cricketFilter) {
        std::puts("warning: --save-rejected is set but --cricket-filter is off — nothing");
        std::puts("         is discarded, so no clips will land in rejected/.");
    }

    // Optional log start-up (silent by default; user can flip --log-level
    // via env or a future flag if they need it). Keeping this branch here
    // rather than always-on mirrors Application::run's discipline: no
    // implicit disk I/O.
    if (cli.cfg.logLevel != echobox::logging::LogLevel::Off) {
        echobox::logging::LoggerConfig logCfg;
        logCfg.dir      = cli.cfg.logDir;
        logCfg.minLevel = cli.cfg.logLevel;
        logCfg.console  = cli.cfg.consoleLog;
        echobox::logging::Logger::instance().start(logCfg);
    }

    scanPlugins();

    echobox::replay::WavFileAudioSource::Params sp;
    sp.files              = std::move(files);
    sp.expectedSampleRate = cli.cfg.sampleRate;
    sp.expectedChannels   = cli.cfg.channels;

    echobox::replay::WavFileAudioSource source(std::move(sp));
    try {
        source.open();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        echobox::logging::Logger::instance().stop();
        return 2;
    }
    if (source.sampleRate() != cli.cfg.sampleRate) {
        cli.cfg.sampleRate = source.sampleRate();
    }

    // Provenance manifest — written up-front to <output>/replay_manifest.json
    // so a scorer that races us on the first sidecar can still see "these
    // clips came from an x86 replay". Sidecars themselves are written by the
    // shipping recorder, which has no notion of "replay"; the manifest is
    // where that annotation lives.
    {
        echobox::replay::RunManifest man;
        man.tool_version = "0.2.0";
#ifdef ECHOBOX_REPLAY_BUILD_TYPE
        man.build_type = ECHOBOX_REPLAY_BUILD_TYPE;
#else
        man.build_type = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
        man.platform = "x86_64-replay";
#elif defined(__aarch64__)
        man.platform = "aarch64-replay";
#else
        man.platform = "unknown-replay";
#endif
#ifdef __FAST_MATH__
        man.fast_math = true;
#endif
#ifdef __VERSION__
        man.compiler = __VERSION__;
#endif
        man.input          = cli.input;
        man.output         = cli.output;
        man.config         = cli.cfg;
        man.had_session_header = haveSessionHeader;
        if (haveSessionHeader) {
            man.session_header = sessionHdr;
            man.config_source  = std::string("session_header:") + sessionHdr.source.string();
        } else {
            man.config_source  = "cli_defaults";
        }
        for (int i = 0; i < argc; ++i) man.cli_argv.emplace_back(argv[i]);
        {
            std::time_t tt = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&tt, &tm);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            man.run_iso8601 = buf;
        }
        try {
            std::error_code ec;
            fs::create_directories(cli.output, ec);
            echobox::replay::writeRunManifest(cli.output / "replay_manifest.json", man);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "warning: failed to write replay_manifest.json: %s\n", e.what());
        }
    }

    LockFreeRingBuffer<float>        dspRing(dspRingCapacity(cli.cfg));
    echobox::recorder::PreRollBuffer preRoll(preRollCapacitySamples(cli.cfg));

    echobox::dsp::DspPipelineConfig dcfg;
    dcfg.sampleRate     = cli.cfg.sampleRate;
    dcfg.fftSize        = cli.cfg.fftSize;
    dcfg.hopSize        = cli.cfg.hopSize;
    dcfg.freqLoHz       = static_cast<float>(cli.cfg.freqLoHz);
    dcfg.freqHiHz       = static_cast<float>(cli.cfg.freqHiHz);
    dcfg.algorithm      = cli.cfg.algorithm;
    dcfg.snrThreshold   = cli.cfg.snrThreshold;
    dcfg.cricketFilter  = cli.cfg.cricketFilter;
    dcfg.extraTrackerTunables = cli.trackerTunables;
    echobox::dsp::DspPipeline dsp(dcfg, dspRing);

    echobox::recorder::RecorderConfig rcfg;
    rcfg.outputDir       = cli.cfg.outputDir;
    rcfg.sampleRate      = cli.cfg.sampleRate;
    rcfg.channels        = cli.cfg.channels;
    rcfg.preRollMs       = cli.cfg.preRollMs;
    rcfg.silenceMs       = cli.cfg.silenceMs;
    rcfg.minLengthMs     = cli.cfg.minLengthMs;
    rcfg.maxLengthMs     = cli.cfg.maxLengthMs;
    rcfg.cricketDiscard  = cli.cfg.cricketFilter;
    switch (cli.cfg.saveRejected) {
        case Config::SaveRejectedMode::Off:
            rcfg.saveRejected = echobox::recorder::SaveRejectedMode::Off; break;
        case Config::SaveRejectedMode::All:
            rcfg.saveRejected = echobox::recorder::SaveRejectedMode::All; break;
        case Config::SaveRejectedMode::Sample:
            rcfg.saveRejected = echobox::recorder::SaveRejectedMode::Sample; break;
        case Config::SaveRejectedMode::Boundary:
            rcfg.saveRejected = echobox::recorder::SaveRejectedMode::Boundary; break;
    }
    rcfg.saveRejectedSampleN    = cli.cfg.saveRejectedSampleN;
    rcfg.saveRejectedMaxPerHour = cli.cfg.saveRejectedMaxPerHour;
    // Replay is not on the field SD card, so the "skip write when disk
    // is low" governor would only get in the way of exhaustive scoring.
    rcfg.saveRejectedMinDiskMb  = 0;

    // Virtual time. Everything the recorder reads as a clock now comes from
    // the audio: the silence timeout, the poll cadence, the WAV filename
    // stamp and the sidecars' boot/capture timestamps. Without this the
    // silenceMs window is measured in host wall time while maxLengthMs is
    // measured in audio frames, and the two drift apart by whatever factor
    // the machine happens to be running at — the defect this tool was
    // reported for. See tools/replay/VirtualClock.hpp.
    echobox::replay::VirtualClock vclock(kVirtualWallEpoch);
    rcfg.clock    = &vclock;
    rcfg.bootWall = kVirtualWallEpoch;

    echobox::recorder::Recorder recorder(rcfg, preRoll, dsp);

    try {
        dsp.start();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: DSP start failed: %s\n", e.what());
        source.close();
        echobox::logging::Logger::instance().stop();
        return 3;
    }

    // --tunable overrides were plumbed through DspPipelineConfig and
    // applied inside dsp.start() before the worker thread was spawned;
    // no post-start work needed here.
    recorder.start();

    std::printf("Replaying %zu file(s) from '%s' → '%s'\n",
                source.fileCount(),
                cli.input.string().c_str(),
                cli.output.string().c_str());
    std::fflush(stdout);

    // Capture loop. Differences from Application::captureLoop:
    //  - EOF from the source is not fatal; we stop instead of
    //    requestExit()+ERROR.
    //  - The push into the DSP ring backpressures (spin+yield) instead of
    //    dropping, so the pipeline is drop-free by construction — the
    //    whole point of replay.
    //  - The feed is paced in recorder poll intervals of *audio* and
    //    interlocked with the DSP and recorder threads, so the run is
    //    deterministic and matches what a real-time device would have
    //    observed. See LockstepFeeder above.
    LockstepFeeder feeder(source, dspRing, preRoll, dsp, vclock,
                          cli.cfg.sampleRate, cli.cfg.hopSize,
                          rcfg.pollIntervalMs);
    const std::uint64_t framesFed = feeder.run();

    // Drain: let the recorder see enough virtual quiet to close any WAV
    // still open when the corpus ran out. Same budget the old wall-clock
    // sleep used (2x silenceMs + margin), but spent in virtual time, so the
    // recorder gets exactly the same number of polls on every run instead
    // of however many a real sleep happened to allow.
    const auto drainMs = std::max<std::uint32_t>(500, rcfg.silenceMs * 2 + 200);
    feeder.drain(drainMs);

    // Release the barrier BEFORE stopping the recorder: its thread is
    // parked waiting for audio that will never arrive, and Recorder::stop()
    // joins it. After release, sleepFor() degrades to a real sleep so the
    // loop notices m_running went false and exits.
    vclock.release();
    recorder.stop();
    dsp.stop();
    source.close();
    echobox::logging::Logger::instance().stop();

    std::printf("Done. %llu samples fed through the pipeline.\n",
                static_cast<unsigned long long>(framesFed));
    return 0;
}
