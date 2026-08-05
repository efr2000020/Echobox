// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// JSON sidecar emitted next to each saved WAV. Carries the per-event
/// diagnostic features the detector logged at trigger/close time plus a
/// snapshot of the noise-floor EMA, so the offline validator can reproduce
/// on-device decisions even on recordings too short to converge the floor
/// from a cold start.

#include "../dsp/ISweepTracker.hpp"   // SidecarPayload, EventFeatures

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace echobox::recorder {

/**
 * @brief Metadata that frames the sidecar — anything the consumer needs to
 *        recreate the detector state the WAV was produced under.
 *
 * Populated by the Recorder at WAV-close time from the @c RecorderConfig and
 * the @c DspPipelineConfig values it was constructed under. The detector's
 * own tunables are appended separately (@c TunableValue list).
 */
struct SidecarRecording {
    std::string             wav_path;        ///< Basename of the WAV (no dir).
    int                     sample_rate{0};
    std::size_t             fft_size{0};
    std::size_t             hop_size{0};
    float                   freq_lo_hz{0.0f};
    float                   freq_hi_hz{0.0f};
    std::uint32_t           preroll_ms{0};
    std::uint32_t           silence_ms{0};
    std::string             algorithm;       ///< e.g. "BandEnergyDetector"
    std::string             boot_iso8601;    ///< Device boot timestamp.
    std::string             capture_iso8601; ///< When the WAV started.

    // --- Rejected-sink metadata (empty for accepted clips) ---
    //
    // Non-empty only when the sidecar is being written next to a rejected
    // clip (--save-rejected). @c rejected_reason is a short tag inferred
    // from the event's own features + the detector's current tunables at
    // close time: "sweep" (sweep-shape gate rejected), "temporal" (temporal
    // repetition-rate guard vetoed), or "unknown" for a plain no-bat-like
    // event that carried no attributable clause. @c rejected_mode is the
    // sink mode string ("all" / "sample" / "boundary") that decided to
    // preserve this clip. Accepted-clip sidecars omit both fields.
    std::string             rejected_reason;
    std::string             rejected_mode;
};

/// Single (key, value) tunable snapshot. The Recorder builds this list by
/// walking the active tracker's listTunables() + getTunable() at close time,
/// so the sidecar pins exactly which knob values produced the recording.
struct TunableValue {
    std::string key;
    double      value{0.0};
    bool        is_int{false};
};

/**
 * Write a sidecar JSON file next to @p wavPath (same basename, @c .json
 * extension). Returns true on success. On failure, leaves no file behind
 * (atomic write via temp + rename).
 *
 * The JSON layout is documented in tools/validator/README.md; the writer
 * is deliberately dependency-free (no nlohmann/json) so the production
 * binary's link surface stays tight.
 */
bool writeSidecar(const std::filesystem::path& wavPath,
                  const SidecarRecording& meta,
                  const std::vector<TunableValue>& tunables,
                  const SidecarPayload& payload);

// --- internal helpers exposed for unit-testability ------------------------

/// Base64-encode a float32 buffer. Each 4-byte float becomes ~5.3 ASCII
/// chars; the result is ASCII-safe and embeds cleanly in a JSON string.
std::string base64EncodeFloats(const std::vector<float>& floats);

/// Inverse of base64EncodeFloats. Returns empty on malformed input.
std::vector<float> base64DecodeFloats(const std::string& b64);

} // namespace echobox::recorder
