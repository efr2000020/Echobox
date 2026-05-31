#pragma once
#include "logging/LogRecord.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace echobox::app {

struct Config {
    // --- Audio capture ---
    std::string     device{"default"};
    int             sampleRate{384000};
    int             channels{1};

    // --- DSP ---
    std::string     algorithm{"BandEnergyDetector"};
    std::size_t     fftSize{4096};
    std::size_t     hopSize{512};
    int             freqLoHz{20000};
    int             freqHiHz{192000};   // Nyquist of the 384 kHz Ultramic.
    // EXPERIMENTAL: coarse sensitivity preset name (e.g. "quiet"/"balanced"/
    // "noisy"). Empty = use the detector's compiled defaults. Each algorithm
    // owns its own preset names; see `--help` and the validator's `describe`
    // command for the list a given plugin supports.
    std::string     sensitivity{};

    // --- Recorder ---
    std::filesystem::path outputDir{"./recordings"};
    std::uint32_t   preRollMs{1000};
    std::uint32_t   silenceMs{2000};
    // Min / max length of the saved WAV (pre-roll + active + hangover, end to
    // end). Recordings shorter than min are deleted instead of finalized;
    // recordings reaching max are closed early. maxLengthMs == 0 means "no
    // cap" so a user who only wants the min filter doesn't have to invent a
    // sentinel max. ConfigValidator enforces that maxLengthMs (when > 0)
    // leaves room for preRollMs + silenceMs, so the defaults below cannot
    // silently degenerate into "always closes before the call".
    std::uint32_t   minLengthMs{0};
    std::uint32_t   maxLengthMs{5000};

    std::filesystem::path logDir{"./logs"};
    // Debug is the default while the false-positive investigation is open so
    // operators don't have to remember --log-level debug to capture the
    // detector heartbeats. Roll back to Info (or Off) for shipping units
    // running unattended for days — debug writes ~half a million lines per
    // day to ./logs and will eventually fill the SD card.
    logging::LogLevel     logLevel{logging::LogLevel::Debug};
};

} // namespace echobox::app
