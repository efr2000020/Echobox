#pragma once
#include "logging/LogRecord.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace litespec::app {

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
    int             freqHiHz{190000};

    // --- Recorder ---
    std::filesystem::path outputDir{"./recordings"};
    std::uint32_t   preRollMs{1000};
    std::uint32_t   silenceMs{2000};

    std::filesystem::path logDir{"./logs"};
    logging::LogLevel     logLevel{logging::LogLevel::Off};
};

} // namespace litespec::app
