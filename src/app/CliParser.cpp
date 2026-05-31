#include "CliParser.hpp"

#include <charconv>
#include <cstdlib>
#include <cstring>
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
        "  --freq-hi-hz <int>        Upper edge of detection window (default: 192000, Nyquist)\n"
        "  --sensitivity <name>      EXPERIMENTAL: coarse sensitivity preset, e.g.\n"
        "                            quiet | balanced | noisy (algorithm-specific)\n"
        "\n"
        "Recorder:\n"
        "  --output-dir <path>       WAV output root (default: ./recordings)\n"
        "  --preroll-ms <int>        Pre-detection audio to include (default: 1000)\n"
        "  --silence-ms <int>        Idle time after last activity before close (default: 2000)\n"
        "  --min-length-ms <int>     Discard recordings shorter than this (default: 0, off)\n"
        "  --max-length-ms <int>     Close recordings reaching this length (default: 5000, 0 = no cap)\n"
        "\n"
        "Logging:\n"
        "  --log-dir <path>          Log output dir (default: ./logs)\n"
        "  --log-level off|debug|info|warn|error   (default: debug)\n"
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
        else if (k == "sensitivity")   cfg.sensitivity = std::string(v);
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
