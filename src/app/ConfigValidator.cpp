// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// ConfigValidator implementation. See ConfigValidator.hpp for the contract.

#include "ConfigValidator.hpp"

#include <cmath>
#include <cstdint>
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

// freqHiHz above Nyquist of the configured sampleRate would be silently
// clamped by the detector; surface it here instead so the user catches a
// mismatched mic/window combination at startup. Typical trigger: user
// switches to a higher-rate mic but forgets to raise --freq-hi-hz, or
// switches to a lower-rate mic without lowering it.
std::optional<std::string> checkFrequencyAgainstNyquist(const Config& cfg) {
    const int nyquist = cfg.sampleRate / 2;
    if (cfg.freqHiHz > nyquist) {
        return "--freq-hi-hz (" + std::to_string(cfg.freqHiHz)
             + ") exceeds Nyquist of --sample-rate (" + std::to_string(cfg.sampleRate)
             + " / 2 = " + std::to_string(nyquist) + ")";
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

// Ordering invariant for the cricket-filter discard path: the recorder
// reads the detector's kept-events counter at endRecording() after
// silenceMs of idle. That read must happen AFTER any in-progress event
// has closed and stamped the counter, or a discard could race an event
// that fires inside the silence window. With the filter OFF nothing
// reads the counter — no race, no floor. With it on we derive the floor
// from the actual settle budget instead of a flat conservative pad.
//
// Since Config::cricketFilter defaults to false, this check is now
// dormant on the shipped configuration — the counter is never read, so
// there is genuinely nothing to guard. Deliberately NOT weakened: it
// still fires for the operator who passes --cricket-filter on, which is
// precisely when the race becomes reachable again, and it is the reason
// they cannot pair that flag with a sub-floor --silence-ms. The shipped
// 20 ms clears the 16 ms floor either way, so turning the filter back on
// needs no other flag.
std::optional<std::string> checkSilenceExceedsHangover(const Config& cfg) {
    if (!cfg.cricketFilter) return std::nullopt;

    // Settle budget the counter needs before endRecording() reads it:
    //   detector hangover  (event close stamps the counter)
    // + recorder poll       (last-active is observed up to one poll late)
    // + safety margin.
    // ASSUMED_HANGOVER_FRAMES must track BandEnergyDetector's compiled
    // hangover_frames default (8). The production app never overrides
    // that tunable; if a future build exposes --hangover-frames, recompute
    // this from the configured value instead.
    constexpr std::uint32_t ASSUMED_HANGOVER_FRAMES = 8;
    constexpr std::uint32_t RECORDER_POLL_MS        = 1;   // Recorder.hpp default
    constexpr std::uint32_t SAFETY_MARGIN_MS        = 4;
    const double frameMs =
        1000.0 * static_cast<double>(cfg.hopSize) / static_cast<double>(cfg.sampleRate);
    const std::uint32_t kMinSilenceMs =
        static_cast<std::uint32_t>(std::ceil(ASSUMED_HANGOVER_FRAMES * frameMs))
        + RECORDER_POLL_MS + SAFETY_MARGIN_MS;
    // Evaluates to 16 ms at the shipping 384 kHz / hop-512 defaults
    // (ceil(8 * 1.333) + 1 + 4). Cut from 40 ms in the short-clip release
    // round: the old 24 ms margin was arbitrary padding and made a
    // ~40 ms end-to-end clip unreachable (max_length must be >= preroll
    // + silence).

    if (cfg.silenceMs < kMinSilenceMs) {
        return "--silence-ms (" + std::to_string(cfg.silenceMs)
             + ") must be at least " + std::to_string(kMinSilenceMs)
             + "ms while --cricket-filter is on, so the discard counter read at "
               "recording close cannot race an event still closing "
               "(the filter is off by default and imposes no floor; this "
               "applies because --cricket-filter on was requested)";
    }
    return std::nullopt;
}

// The cap must leave room for pre-roll *and* the trailing silence window:
// the recorder writes pre-roll, then the active call, then keeps writing
// for `silenceMs` of trailing quiet before closing. If max < pre-roll +
// silence the recorder is guaranteed to close before the detected call can
// play out, producing files with no usable audio.
// --save-rejected is a strict superset of --cricket-filter; if the filter
// is off the recorder never enters the discard branch, so no clip would
// ever be routed to the rejected sink. Surface the misconfiguration
// instead of silently doing nothing.
//
// Now that the filter defaults OFF, --save-rejected on its own is an
// error rather than the working combination it used to be, so the
// message spells out the extra flag the operator now has to pass.
std::optional<std::string> checkSaveRejectedRequiresFilter(const Config& cfg) {
    if (cfg.saveRejected == Config::SaveRejectedMode::Off) return std::nullopt;
    if (!cfg.cricketFilter) {
        return "--save-rejected requires --cricket-filter on (with the filter "
               "off, no clip is ever cricket-discarded and nothing would ever "
               "be written to rejected/); the filter is off by default now, so "
               "pass --cricket-filter on alongside --save-rejected";
    }
    return std::nullopt;
}

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
    run(checkFrequencyAgainstNyquist(cfg));
    run(checkLengthOrder(cfg));
    run(checkRecordingBudget(cfg));
    run(checkSilenceExceedsHangover(cfg));
    run(checkSaveRejectedRequiresFilter(cfg));

    return errors;
}

} // namespace echobox::app
