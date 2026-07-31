// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Interface the Recorder uses to publish clip-level save/discard
/// decisions to the collection overlay. When the overlay is off,
/// Recorder holds a null pointer and no code path fires (kill-switch).
///
/// The recorded decisions are the *ground truth* for §4.1's
/// recorder-model cross-check: the shadow "would_save_R2v3" column of
/// the decision log. The offline verifier diffs @c recorder_model.py
/// against these values, clip-for-clip, on real field data.

#include <cstdint>
#include <string>

namespace echobox::collection {

/**
 * @brief One decision the shipping Recorder made about one clip.
 *
 * Sample indices are absolute (from capture start), consistent with the
 * SampleClock used by Stream A and the event log. That makes offline
 * correlation "clip covers events whose start_sample ∈ [clip_start,
 * clip_end]" a direct arithmetic check.
 *
 * @c reason is one of:
 *   - "saved"         — the WAV was finalised
 *   - "min-length"    — dropped: shorter than @c minLengthMs
 *   - "cricket-gate"  — dropped: no bat-like event in the window
 *   - "max-len-active"— cap tripped mid-event, clip kept per §2.3 guard
 *                        (informational; treated as "saved" for §4.1
 *                        cross-check purposes)
 */
struct RecorderDecision {
    std::uint64_t clip_start_sample{0};
    std::uint64_t clip_end_sample{0};
    std::uint64_t bat_like_at_start{0};
    std::uint64_t bat_like_at_end{0};
    std::uint32_t duration_ms{0};
    float         event_lo_hz{0.0f};
    float         event_hi_hz{0.0f};
    bool          saved{false};
    std::string   reason;
};

/// Interface the Recorder holds a pointer to. Null pointer ⇒ overlay off,
/// nothing to publish. Implementations must be thread-safe: called from
/// the Recorder's own thread.
class IRecorderDecisionSink {
public:
    virtual ~IRecorderDecisionSink() = default;
    virtual void onRecorderDecision(const RecorderDecision& d) = 0;
};

} // namespace echobox::collection
