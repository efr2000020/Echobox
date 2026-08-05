// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// CliParser implementation. See CliParser.hpp for the public contract.

#include "CliParser.hpp"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace echobox::app {

namespace {

bool parseInt(std::string_view sv, long& out) {
    if (sv.empty()) return false;
    auto* first = sv.data();
    auto* last  = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

bool parseUInt(std::string_view sv, std::uint32_t& out) {
    long v = 0;
    if (!parseInt(sv, v) || v < 0) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

bool parseFloat(std::string_view sv, float& out) {
    if (sv.empty()) return false;
    // std::from_chars for float is C++17 in spec, missing in libstdc++ before
    // GCC 11. Use std::strtof and verify the entire token was consumed.
    std::string s(sv);
    char* end = nullptr;
    errno = 0;
    const float v = std::strtof(s.c_str(), &end);
    if (errno != 0) return false;
    if (end == s.c_str() || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

struct Option {
    std::string_view key;
    std::string_view value;
    bool             consumedNext;
};

// Splits "--key=value" or pairs "--key value". Returns false when argv[i] is
// not an option-shaped token.
bool nextOption(int argc, char** argv, int& i, Option& out) {
    std::string_view tok(argv[i]);
    if (tok.size() < 2 || tok[0] != '-' || tok[1] != '-') return false;
    auto rest = tok.substr(2);
    auto eq = rest.find('=');
    if (eq != std::string_view::npos) {
        out.key   = rest.substr(0, eq);
        out.value = rest.substr(eq + 1);
        out.consumedNext = false;
    } else {
        out.key = rest;
        if (i + 1 < argc && argv[i + 1][0] != '-') {
            out.value = argv[i + 1];
            out.consumedNext = true;
        } else {
            out.value = {};
            out.consumedNext = false;
        }
    }
    return true;
}

CliResult err(std::string msg) {
    CliResult r;
    r.kind = CliResult::Kind::Error;
    r.errorMessage = std::move(msg);
    return r;
}

} // namespace

const char* versionText() {
    return "Echobox 0.2.0\n";
}

const char* helpText() {
    return
        "Echobox — bat presence detector + recorder gate.\n"
        "\n"
        "Usage: Echobox [options]\n"
        "\n"
        "Listens to an ultrasonic microphone and saves a WAV recording whenever\n"
        "a bat call is detected, including a short lead-in before each call.\n"
        "\n"
        "Example:\n"
        "  Echobox --device plughw:CARD=UltraMic384K --output-dir ./recordings\n"
        "\n"
        "Audio capture:\n"
        "  --device <name>           ALSA capture device (default: \"default\")\n"
        "  --sample-rate <hz>        Capture sample rate (default: 384000)\n"
        "\n"
        "Detection:\n"
        "  --algorithm <name>        Detector plugin (default: BandEnergyDetector)\n"
        "  --fft-size <int>          FFT size (default: 4096)\n"
        "  --hop-size <int>          FFT hop in samples (default: 512)\n"
        "  --freq-lo-hz <int>        Lower edge of detection window (default: 20000)\n"
        "  --freq-hi-hz <int>        Upper edge of detection window; must not exceed Nyquist\n"
        "                            of --sample-rate (default: 192000, the Nyquist of 384 kHz)\n"
        "  --snr-threshold <float>   SNR ratio above which a frame counts as a\n"
        "                            detection. Lower = more sensitive (more recall,\n"
        "                            more false positives); higher = stricter\n"
        "                            (default: 12.0)\n"
        "\n"
        "Recorder:\n"
        "  --output-dir <path>       WAV output root (default: ./recordings)\n"
        "  --preroll-ms <int>        Pre-detection audio to include (default: 50)\n"
        "  --silence-ms <int>        Idle time after last activity before close (default: 50).\n"
        "                            With --cricket-filter on, this must be >= a derived\n"
        "                            floor (~40 ms at the shipped 384 kHz / hop-512) so\n"
        "                            the discard counter read cannot race an event still\n"
        "                            closing; --cricket-filter off removes the floor.\n"
        "  --min-length-ms <int>     Discard recordings shorter than this (default: 0, off)\n"
        "  --max-length-ms <int>     Close recordings reaching this length (default: 200, 0 = no cap).\n"
        "                            For R1/R2v2-style long clips, pass e.g.\n"
        "                            --preroll-ms 1000 --silence-ms 2000 --max-length-ms 5000.\n"
        "\n"
        "Cricket filter:\n"
        "  --cricket-filter on|off   Master switch for the cricket-rejection filter.\n"
        "                            When on (default), the detector's sweep-shape gate\n"
        "                            AND the recorder's post-hoc no-bat-like-event\n"
        "                            discard both engage. When off, both halves are\n"
        "                            disabled — the field escape hatch if a site\n"
        "                            produces bat calls the gate can't characterise.\n"
        "                            (default: on)\n"
        "\n"
        "Rejected capture (observability — off = byte-identical to today):\n"
        "  --save-rejected off|all|sample|boundary\n"
        "                            Preserve cricket-discarded clips under\n"
        "                            <output>/rejected/ so they can be reviewed later.\n"
        "                              off      no rejected/ dir is created (default)\n"
        "                              all      save every rejected clip (local/offline\n"
        "                                       validation; storage-heavy)\n"
        "                              sample   save a random 1-in-N\n"
        "                              boundary save only near-threshold near-misses\n"
        "                                       (highest-value field mode)\n"
        "  --save-rejected-sample-n <int>       1-in-N sampling ratio for mode 'sample'\n"
        "                                       (default: 500; must be >= 1)\n"
        "  --save-rejected-max-per-hour <int>   Storage governor: cap on rejected clips\n"
        "                                       written per rolling hour. 0 = unlimited\n"
        "                                       (for local 'all' runs). (default: 200)\n"
        "\n"
        "Logging:\n"
        "  --log-dir <path>          Log output dir (default: ./logs)\n"
        "  --log-level off|debug|info|warn|error   (default: off)\n"
        "  --console-log on|off      Mirror log records to stderr (default: on)\n"
        "  --heartbeat-sec <int>     Emit an app:HEARTBEAT line every N seconds\n"
        "                            (default: 60; 0 disables)\n"
        "\n"
        "  -h, --help                Show this help and exit\n"
        "  -V, --version             Print version and exit\n";
}

CliResult parseCli(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "-h" || a == "--help") {
            CliResult r; r.kind = CliResult::Kind::HelpRequested; return r;
        }
        if (a == "-V" || a == "--version") {
            CliResult r; r.kind = CliResult::Kind::VersionRequested; return r;
        }

        Option opt{};
        if (!nextOption(argc, argv, i, opt)) {
            return err(std::string("unexpected argument: ") + argv[i]);
        }
        if (opt.value.empty() && opt.key != "help" && opt.key != "version") {
            return err("missing value for --" + std::string(opt.key));
        }
        if (opt.consumedNext) ++i;

        const auto& k = opt.key;
        const auto& v = opt.value;

        if      (k == "device")        cfg.device = std::string(v);
        else if (k == "sample-rate") {
            long x; if (!parseInt(v, x) || x <= 0) return err("invalid --sample-rate");
            cfg.sampleRate = static_cast<int>(x);
        }
        else if (k == "algorithm")     cfg.algorithm   = std::string(v);
        else if (k == "snr-threshold") {
            float x; if (!parseFloat(v, x) || !(x > 0.0f)) return err("invalid --snr-threshold (must be > 0)");
            cfg.snrThreshold = x;
        }
        else if (k == "fft-size") {
            long x; if (!parseInt(v, x) || x <= 0) return err("invalid --fft-size");
            cfg.fftSize = static_cast<std::size_t>(x);
        }
        else if (k == "hop-size") {
            long x; if (!parseInt(v, x) || x <= 0) return err("invalid --hop-size");
            cfg.hopSize = static_cast<std::size_t>(x);
        }
        else if (k == "freq-lo-hz") {
            long x; if (!parseInt(v, x) || x < 0) return err("invalid --freq-lo-hz");
            cfg.freqLoHz = static_cast<int>(x);
        }
        else if (k == "freq-hi-hz") {
            long x; if (!parseInt(v, x) || x < 0) return err("invalid --freq-hi-hz");
            cfg.freqHiHz = static_cast<int>(x);
        }
        else if (k == "output-dir")    cfg.outputDir = std::string(v);
        else if (k == "preroll-ms") {
            std::uint32_t u; if (!parseUInt(v, u)) return err("invalid --preroll-ms");
            cfg.preRollMs = u;
        }
        else if (k == "silence-ms") {
            std::uint32_t u; if (!parseUInt(v, u)) return err("invalid --silence-ms");
            cfg.silenceMs = u;
        }
        else if (k == "min-length-ms") {
            std::uint32_t u; if (!parseUInt(v, u)) return err("invalid --min-length-ms");
            cfg.minLengthMs = u;
        }
        else if (k == "max-length-ms") {
            std::uint32_t u; if (!parseUInt(v, u)) return err("invalid --max-length-ms");
            cfg.maxLengthMs = u;
        }
        else if (k == "log-dir")       cfg.logDir = std::string(v);
        else if (k == "log-level") {
            logging::LogLevel lvl;
            if (!logging::parseLevel(std::string(v).c_str(), lvl)) {
                return err("invalid --log-level (use debug|info|warn|error)");
            }
            cfg.logLevel = lvl;
        }
        else if (k == "cricket-filter") {
            if      (v == "on"  || v == "1" || v == "true")  cfg.cricketFilter = true;
            else if (v == "off" || v == "0" || v == "false") cfg.cricketFilter = false;
            else return err("invalid --cricket-filter (use on|off)");
        }
        else if (k == "console-log") {
            if      (v == "on"  || v == "1" || v == "true")  cfg.consoleLog = true;
            else if (v == "off" || v == "0" || v == "false") cfg.consoleLog = false;
            else return err("invalid --console-log (use on|off)");
        }
        else if (k == "heartbeat-sec") {
            std::uint32_t u; if (!parseUInt(v, u)) return err("invalid --heartbeat-sec");
            cfg.heartbeatSec = u;
        }
        else if (k == "save-rejected") {
            if      (v == "off")      cfg.saveRejected = Config::SaveRejectedMode::Off;
            else if (v == "all")      cfg.saveRejected = Config::SaveRejectedMode::All;
            else if (v == "sample")   cfg.saveRejected = Config::SaveRejectedMode::Sample;
            else if (v == "boundary") cfg.saveRejected = Config::SaveRejectedMode::Boundary;
            else return err("invalid --save-rejected (use off|all|sample|boundary)");
        }
        else if (k == "save-rejected-sample-n") {
            std::uint32_t u;
            if (!parseUInt(v, u) || u == 0) return err("invalid --save-rejected-sample-n (must be >= 1)");
            cfg.saveRejectedSampleN = u;
        }
        else if (k == "save-rejected-max-per-hour") {
            std::uint32_t u; if (!parseUInt(v, u)) return err("invalid --save-rejected-max-per-hour");
            cfg.saveRejectedMaxPerHour = u;
        }
        else {
            return err("unknown option --" + std::string(k));
        }
    }

    // Cross-flag invariants (freq-lo < freq-hi, max-length budget, etc.)
    // live in ConfigValidator so the same module catches every "two flags
    // that disagree" case in one place. CliParser only enforces per-flag
    // bounds — i.e. that each value parses and is in its own legal range.
    return {};
}

} // namespace echobox::app
