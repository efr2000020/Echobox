// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Interface the Recorder uses to publish clip-level save/discard
/// decisions to the collection overlay. When the overlay is off,
/// Recorder holds a null pointer and no code path fires (kill-switch).
///
/// What this is, in the plan's terms
/// ---------------------------------
/// The plan (§2.2 C, §4.1) calls this the "shadow @c would_save_R2v3"
/// column. That name is now retired, for two reasons, and the record
/// itself is kept:
///
///  - It was never a JSON field. "would_save_R2v3" was the plan's name
///    for THIS record — the real firmware's own save/discard verdict,
///    published so §4.1 could diff a Python model against it.
///  - "R2v3" named the sweep-shape-gate release. That gate is gone,
///    replaced by the confident-reject noise rule, so a field labelled
///    with it would describe a decision this firmware no longer makes.
///
/// The record survives the rename because it is not a shadow of anything:
/// it is the shipping Recorder's actual verdict on an actual clip, which
/// is still the only device-side ground truth for what the recorder half
/// of the cricket filter did. Its consumer changed — @c recorder_model.py
/// is stale and @c tools/session_screen drives the real C++ through
/// @c echobox-replay instead — but the artefact is the same one.

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
 *   - "saved"          — the WAV was finalised
 *   - "min-length"     — dropped: shorter than @c minLengthMs
 *   - "cricket-gate"   — dropped: no bat-like event in the window
 *   - "cricket-gate-rejected-sink" — same verdict as "cricket-gate", but
 *                        @c --save-rejected preserved the clip under
 *                        @c rejected/ instead of aborting it. Still
 *                        @c saved=false: the clip is not a detection.
 *   - "max-len-active" — cap tripped mid-event, clip kept because the
 *                        triggering event had not yet stamped the
 *                        kept-events counter (informational; the clip
 *                        WAS saved)
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
    /// Which gate clause attributed the rejection, for the two reasons
    /// that carry one: "temporal" (rep-rate veto) or "noise" (confident-
    /// reject rule), or "unknown" when the clip held no closed event.
    /// Empty on the save paths. This is the current-rule replacement for
    /// the plan's "which clause fired" (§2.2 C), whose original
    /// vocabulary — bandwidth / sweep — described the retired gate.
    /// Sourced from the same @c Recorder::classifyRejection the rejected-
    /// clip sidecar uses, so the two never disagree.
    std::string   clause;
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
