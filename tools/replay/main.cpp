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

    echobox::recorder::Recorder recorder(rcfg, preRoll, dsp);

    try {
        dsp.start();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: DSP start failed: %s\n", e.what());
        source.close();
        echobox::logging::Logger::instance().stop();
        return 3;
    }
    recorder.start();

    std::printf("Replaying %zu file(s) from '%s' → '%s'\n",
                source.fileCount(),
                cli.input.string().c_str(),
                cli.output.string().c_str());
    std::fflush(stdout);

    // Capture loop. Differences from Application::captureLoop:
    //  - EOF from the source is not fatal; we break instead of
    //    requestExit()+ERROR.
    //  - The push into the DSP ring backpressures (spin+yield) instead of
    //    dropping, so the pipeline is drop-free by construction — the
    //    whole point of replay.
    std::array<std::int16_t, kCaptureChunkFrames> intBuf{};
    std::array<float, kCaptureChunkFrames>        floatBuf{};
    constexpr float kInvScale = 1.0f / 32768.0f;

    std::uint64_t framesFed = 0;
    while (true) {
        int got = source.read(std::span<std::int16_t>(intBuf));
        if (got < 0) break; // WavFileAudioSource never returns <0 today, but
                            // keep the guard aligned with IAudioSource's
                            // documented contract.
        if (got == 0) {
            if (source.finished()) break;
            // Between-file rollover: give the DSP a nudge and try again.
            dsp.notifyInput();
            continue;
        }

        const std::size_t n = static_cast<std::size_t>(got);
        preRoll.write(std::span<const std::int16_t>(intBuf.data(), n));
        for (std::size_t i = 0; i < n; ++i) {
            floatBuf[i] = static_cast<float>(intBuf[i]) * kInvScale;
        }
        // Drop-free push: back off until the DSP thread makes room. This is
        // the entire mechanism that turns "faster-than-real-time" from
        // "faster and lossier" into "faster and complete".
        for (std::size_t i = 0; i < n; ++i) {
            while (!dspRing.push(floatBuf[i])) {
                dsp.notifyInput();
                std::this_thread::yield();
            }
        }
        dsp.notifyInput();
        framesFed += n;
    }

    // Drain: give the DSP a moment to consume the tail of the ring, and the
    // recorder a hangover window to close any in-progress WAV cleanly. The
    // recorder's poll cadence + silenceMs bound how long "in-progress"
    // survives after the last active hop; 2x silenceMs + a small margin is
    // enough in practice.
    const auto drainMs = std::max<std::uint32_t>(500, rcfg.silenceMs * 2 + 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(drainMs));

    recorder.stop();
    dsp.stop();
    source.close();
    echobox::logging::Logger::instance().stop();

    std::printf("Done. %llu samples fed through the pipeline.\n",
                static_cast<unsigned long long>(framesFed));
    return 0;
}
