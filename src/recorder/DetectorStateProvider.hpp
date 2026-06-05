// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Read-only handoff from the DSP pipeline to the recorder thread. Pure data
/// + a one-method interface; no implementations live here.

#include "../dsp/ISweepTracker.hpp"   // SidecarPayload

#include <cstdint>
#include <string>
#include <vector>

namespace echobox::recorder {

struct TunableValue;  // defined in Sidecar.hpp; forward-declared so the
                      // header chain stays one-way (sidecar consumers know
                      // about the provider, not the other way round).

/**
 * @brief One sample of the detector's current state.
 *
 * @c loHz / @c hiHz describe the firing sub-band's current frequency span.
 * They are meaningful only while @c active is @c true; readers should treat
 * them as undefined when @c active is @c false.
 */
struct DetectorStateSnapshot {
    bool          active;
    float         loHz;
    float         hiHz;
    /// Monotonic count of active→inactive transitions since startup. Lets a
    /// polling consumer detect a complete event that opened and closed
    /// between two polls without seeing @c active flip.
    std::uint64_t generation;
};

/**
 * @brief Wait-free view of the DSP pipeline's current detector state.
 *
 * Polled by the Recorder thread at a few-millisecond cadence.
 *
 * @warning @c snapshot() MUST be wait-free — no mutexes, no allocations,
 *          no syscalls. The Recorder may call it concurrently with the DSP
 *          thread publishing new state. The sidecar-related methods below
 *          are off the hot path (called at WAV close), so they may
 *          allocate / take short locks.
 */
class IDetectorStateProvider {
public:
    virtual ~IDetectorStateProvider() = default;
    virtual DetectorStateSnapshot snapshot() const = 0;

    // --- Sidecar diagnostics (off the audio hot loop) ---

    /// Drain all completed event features + the noise-floor snapshot since
    /// the last call. Default: no-op for providers without a tracker handle.
    virtual bool drainSidecarPayload(SidecarPayload& /*out*/) { return false; }

    /// Snapshot the active algorithm's tunable values (key + numeric value +
    /// type). Used by the Recorder to pin "what knobs produced this WAV"
    /// into the sidecar. Returns false if no tracker is loaded.
    virtual bool currentTunables(std::vector<TunableValue>& /*out*/) const { return false; }

    /// Name of the currently-loaded algorithm. Empty when no tracker is loaded.
    virtual std::string algorithmName() const { return {}; }

    /// FFT and hop sizes the pipeline is running with. Recorder copies these
    /// into the sidecar so the validator can reproduce the STFT exactly.
    virtual std::size_t fftSize() const { return 0; }
    virtual std::size_t hopSize() const { return 0; }
    virtual float       freqLoHz() const { return 0.0f; }
    virtual float       freqHiHz() const { return 0.0f; }
};

} // namespace echobox::recorder
