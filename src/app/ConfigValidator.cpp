// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// ConfigValidator implementation. See ConfigValidator.hpp for the contract.

#include "ConfigValidator.hpp"

#include <optional>
#include <string>

namespace echobox::app {

namespace {

// ---- Invariant checks --------------------------------------------------
//
// Each helper returns std::nullopt when the invariant holds, or a complete
// human-readable error message describing what went wrong and which flags
// are in conflict. Messages should name the actual flag the user passed so
// fixing the problem is mechanical, not a guessing game.

std::optional<std::string> checkFrequencyWindow(const Config& cfg) {
    if (cfg.freqLoHz >= cfg.freqHiHz) {
        return "--freq-lo-hz (" + std::to_string(cfg.freqLoHz)
             + ") must be less than --freq-hi-hz (" + std::to_string(cfg.freqHiHz) + ")";
    }
    return std::nullopt;
}

std::optional<std::string> checkLengthOrder(const Config& cfg) {
    if (cfg.maxLengthMs > 0 && cfg.minLengthMs > cfg.maxLengthMs) {
        return "--min-length-ms (" + std::to_string(cfg.minLengthMs)
             + ") cannot exceed --max-length-ms (" + std::to_string(cfg.maxLengthMs) + ")";
    }
    return std::nullopt;
}

// The cap must leave room for pre-roll *and* the trailing silence window:
// the recorder writes pre-roll, then the active call, then keeps writing
// for `silenceMs` of trailing quiet before closing. If max < pre-roll +
// silence the recorder is guaranteed to close before the detected call can
// play out, producing files with no usable audio.
std::optional<std::string> checkRecordingBudget(const Config& cfg) {
    if (cfg.maxLengthMs == 0) return std::nullopt;   // cap disabled
    const std::uint32_t minNeeded = cfg.preRollMs + cfg.silenceMs;
    if (cfg.maxLengthMs < minNeeded) {
        return "--max-length-ms (" + std::to_string(cfg.maxLengthMs)
             + ") must be at least --preroll-ms (" + std::to_string(cfg.preRollMs)
             + ") + --silence-ms (" + std::to_string(cfg.silenceMs)
             + ") = " + std::to_string(minNeeded)
             + "ms, otherwise every recording closes before the detected call ends";
    }
    return std::nullopt;
}

} // namespace

std::vector<std::string> validateConfig(const Config& cfg) {
    std::vector<std::string> errors;
    auto run = [&](std::optional<std::string> result) {
        if (result) errors.push_back(std::move(*result));
    };

    // Add new invariants here. Order is only relevant for the order in which
    // they appear in the error report; every check runs regardless.
    run(checkFrequencyWindow(cfg));
    run(checkLengthOrder(cfg));
    run(checkRecordingBudget(cfg));

    return errors;
}

} // namespace echobox::app
