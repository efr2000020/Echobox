// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Single Config struct that holds every operator-tunable option for the
/// production binary. Populated by CliParser, validated by ConfigValidator,
/// then handed to Application which fans the fields out to each subsystem.

#include "logging/LogRecord.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace echobox::app {

/**
 * @brief Runtime configuration for the production binary.
 *
 * Every field has a sane default that matches a shipping field unit; the CLI
 * exposes a flag for each. Defaults that must stay in lock-step with another
 * module (e.g. @c snrThreshold tracking @c BandEnergyDetector's compiled
 * default) are called out in the per-field comments below.
 */
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
    int             freqHiHz{192000};   // Default suits the 384 kHz Ultramic
                                        // (=its Nyquist). For other mics, set
                                        // --sample-rate and --freq-hi-hz
                                        // together; ConfigValidator enforces
                                        // freqHiHz <= sampleRate / 2.
    // SNR threshold used by the active detector to decide a frame is "hot".
    // Plumbed through to the tracker as the `band_snr_threshold` tunable. The
    // default below must track BandEnergyDetector's compiled default so a user
    // who never passes --snr-threshold sees identical behavior to before this
    // flag existed; the two defaults are linked by ConfigValidator-style note,
    // not by build-time wiring, so update both together.
    float           snrThreshold{12.0f};

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
    // Off by default: a shipped unit running unattended writes nothing to
    // disk unless the operator opts in. Application::run treats Off as
    // "don't even open a log file", so the SD card sees no I/O from the
    // logging subsystem. For field debugging pass --log-level info (or
    // debug). The console sink honours the same level.
    logging::LogLevel     logLevel{logging::LogLevel::Off};

    // Console sink (stderr, human-readable) mirrors the JSON-lines file
    // sink. Set to false to run headless without a terminal handler
    // attached.
    bool                  consoleLog{true};
    // Emit an "app: HEARTBEAT ..." record every this-many seconds so a
    // field operator can prove the unit is alive from a log tail. 0
    // disables.
    std::uint32_t         heartbeatSec{60};

    // Cricket filter master switch. On (default) enables BOTH halves of
    // the filter under a single flag:
    //   * detector's sweep_gate_enabled sweep-shape gate + temporal guard;
    //   * recorder's cricketDiscard "no bat-like event" post-hoc drop.
    // Off disables both → recorder behaves as if no filter existed. The
    // field escape hatch: if a deployment site produces bat calls the
    // gate can't characterise, one flag turns the whole filter off.
    bool                  cricketFilter{true};
};

} // namespace echobox::app
