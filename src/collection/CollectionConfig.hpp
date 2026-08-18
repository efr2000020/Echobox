// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Runtime configuration for the data-collection firmware overlay.
///
/// This overlay is a *validation-only* superset of the shipping recorder:
/// with @c enabled=false it must be a byte-identical no-op (nothing
/// constructed, no threads spawned, no code paths reached in the audio hot
/// loop). The default is off. The plan doc that motivates this module is
/// private_docs/plans/03_DATA_COLLECTION_IMPL_VALIDATION_PLAN.md.

#include <cstdint>
#include <filesystem>

namespace echobox::collection {

/**
 * @brief Master switches for the four collection streams.
 *
 * Streams:
 *   A — continuous reference audio (pre-detector tap; §2.2 A);
 *   B — per-event WAV clips, saved for BOTH accepted and rejected events
 *       (§2.2 B);
 *   C — per-event JSONL decision log, plus the recorder's own per-clip
 *       save/discard verdict (§2.2 C — the plan calls that verdict the
 *       shadow @c would_save_R2v3; see RecorderDecisionSink.hpp for why
 *       the name is retired and the artefact is not);
 *   D — operational-log passthrough (§2.2 D). Landed as-is via the existing
 *       operational log; this flag is a documentation anchor rather than a
 *       code toggle in this commit.
 *
 * Each stream is independently enable-able so a small SD card can carry
 * more collection hours by dropping the largest (Stream A).
 */
struct StreamToggles {
    bool streamA{true};
    bool streamB{true};
    bool streamC{true};
    bool streamD{true};
};

/**
 * @brief Governor thresholds: when to stop cleanly.
 *
 * Governor trips when EITHER of the following is reached:
 *   - @c maxDurationSec elapsed since capture start (0 = no cap); or
 *   - free space on the collection filesystem drops below
 *     @c minFreeMbFloor.
 *
 * On trip, the session closes the in-progress chunk, flushes decision
 * logs, and writes a @c SESSION_END marker so a partial run is fully
 * trustworthy.
 */
struct GovernorConfig {
    std::uint32_t  maxDurationSec{0};        // 0 = no cap; run until the card fills
    std::uint64_t  minFreeMbFloor{100};      // stop before we corrupt a tail
    std::uint32_t  pollIntervalSec{10};      // how often the governor thread wakes
};

struct CollectionConfig {
    /// Master switch. false ⇒ nothing in this module runs. Kill-switch
    /// discipline: no observer registered, no thread spawned, no audio-tap
    /// buffer allocated.
    bool                   enabled{false};

    /// Root directory for all collection artefacts (session header, chunk
    /// manifest, WAV chunks, event clips, decision log, session-end marker).
    /// Independent of the shipping recorder's @c outputDir so a mounted
    /// dedicated collection card can be pointed at without disturbing the
    /// production output tree.
    std::filesystem::path  dir{"./collection"};

    StreamToggles          streams{};
    GovernorConfig         governor{};

    /// Cadence, in seconds, of the periodic per-bin noise-floor snapshot
    /// written to Stream C as {"kind":"noise_floor",...}. 0 disables it.
    ///
    /// Why this exists at all: the detector's noise floor is what decides
    /// whether a faint call clears threshold, and until now it was captured
    /// only once per saved clip. That makes "the call was too faint" and
    /// "the background pushed the floor up" indistinguishable across a whole
    /// night — an ambiguity that limited a recent recall audit — and it fails
    /// worst exactly where it matters, because interference loud enough to
    /// mask bats also suppresses the clips that would have carried a floor
    /// snapshot. A timed trace does not depend on detections happening.
    ///
    /// Why 60 s: the floor is a slow EMA driven by ambient conditions, so
    /// the trace only has to resolve the timescale on which a chorus starts,
    /// saturates and stops — minutes, not seconds. 60 s costs ~11 kB per
    /// snapshot (2049 bins x 4 B, base64), i.e. ~600 snapshots and ~6.5 MB
    /// over a 10 h night: a rounding error beside Stream A's ~2.76 GB/h, and
    /// small enough that decisions.jsonl stays comfortably greppable.
    /// Exposed as --collection-noise-floor-sec so a field session that wants
    /// finer resolution can have it without a cross-compile.
    std::uint32_t          noiseFloorIntervalSec{60};

    /// Free-form site note stamped into the session header for later
    /// provenance (e.g. "site=oak-woodland-N; mic=UltraMic384K; gain=+40dB").
    std::string            siteNote{};
};

} // namespace echobox::collection
