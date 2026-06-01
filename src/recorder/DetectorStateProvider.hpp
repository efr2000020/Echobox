// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Read-only handoff from the DSP pipeline to the recorder thread. Pure data
/// + a one-method interface; no implementations live here.

#include <cstdint>

namespace echobox::recorder {

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
 * @warning Implementations MUST be wait-free — no mutexes, no allocations,
 *          no syscalls. The Recorder may call @c snapshot() concurrently
 *          with the DSP thread publishing new state.
 */
class IDetectorStateProvider {
public:
    virtual ~IDetectorStateProvider() = default;
    virtual DetectorStateSnapshot snapshot() const = 0;
};

} // namespace echobox::recorder
