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
/// private_docs/plans/DATA_COLLECTION_IMPL_VALIDATION_PLAN.md.

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
 *   C — per-event JSONL decision log with shadow @c would_save_R2v3
 *       (§2.2 C);
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

    /// Free-form site note stamped into the session header for later
    /// provenance (e.g. "site=oak-woodland-N; mic=UltraMic384K; gain=+40dB").
    std::string            siteNote{};
};

} // namespace echobox::collection
