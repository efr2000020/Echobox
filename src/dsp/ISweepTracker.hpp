// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Public detector-plugin ABI. Every detector — built-in or @c dlopen'd —
/// implements @c ISweepTracker and exports the three plugin entry points at
/// the bottom of this file. Layout-compatible structs are mirrored in
/// @c tools/validator/native/validator_c_api.h for the Python FFI side.

#include <span>
#include <cstddef>
#include <cstdint>
#include <vector>

// Stable, pre-existing detection event record. Emitted when a hot region closes.
#pragma pack(push, 1)
struct Annotation {
    uint32_t start_frame;
    uint32_t end_frame;
    float high_freq;
    float low_freq;
};
#pragma pack(pop)

// Per-event diagnostic features captured at the moment a hot region opens
// (trigger_snr / trigger_flatness / band_index) and finalised at close
// (end_frame / peak_snr / duration_frames). Mirrors the fields the detector
// already logs via LS_INFO at event start/end, but as structured data that
// the recorder can serialise into a sidecar without parsing log lines.
//
// The trailing sweep-shape block (bandwidth_khz, drift_khz, path_ratio,
// mono_fraction, gate_rejected) is populated by detectors that implement
// the cricket false-positive gate; default-zero is meaningful for older
// detectors that don't (the validator treats all-zero sweep features as
// "not computed"). Note that in BandEnergyDetector the four sweep-shape
// features are DIAGNOSTICS as of the confident-reject gate — they are
// still computed and emitted, but gate_rejected is decided from
// trigger_snr / trigger_flatness / band_index instead.
//
// Plain old data; EventFeatures is NOT mirrored in the validator C API today —
// it travels device-to-validator via the JSON sidecar, not the C FFI struct.
struct EventFeatures {
    uint32_t start_frame;
    uint32_t end_frame;
    uint16_t duration_frames;
    int16_t  band_index;        // 1-based; -1 if not applicable
    float    trigger_snr;
    float    trigger_flatness;
    float    peak_snr;
    float    lo_hz;
    float    hi_hz;
    // --- sweep-shape features (per-frame dominant-bin statistics) ---
    float    bandwidth_khz;     // 10-dB bandwidth at the dominant frame
    float    drift_khz;         // dominant-bin frequency excursion over the event
    float    path_ratio;        // sum(|Δbin|) / max(1, range); ~1 sweep, ~2 hopping
    float    mono_fraction;     // max(#up,#down) / (#up+#down)
    bool     gate_rejected;     // the binding verdict: detector dropped this event
    // --- decision-path diagnostics (populated by BandEnergyDetector; older
    //     detectors leave these zero. Observability only — filter behaviour
    //     is unchanged whether these are populated or not.) ---
    bool     sweep_bat_like;    // what the RETIRED sweep-shape verdict would have said
    bool     veto_applied;      // temporal rep-guard flipped a keep into a reject
    bool     provisional_rejected;  // RETIRED with the two-tier split; always false now
    float    rep_rate_hz;       // onset ring's estimated rate at gate-decision time
    float    rep_cv;            // onset ring's CV(IDI) at gate-decision time
    uint16_t rep_n_onsets;      // onset ring occupancy at gate-decision time
};

// Bundle of state the recorder drains from the tracker when a WAV closes,
// for emission as a JSON sidecar next to the WAV. The sidecar lets the
// offline validator reproduce on-device decisions even on very short
// recordings, where its EMA noise floor would otherwise be cold-started
// from the first frame.
//
// `noise_floor_at_first_event` is a snapshot of the per-bin noise floor
// taken at the moment of the FIRST event opened since the last drain;
// it has `fftSize/2 + 1` entries (or is empty if no event fired). The
// recorder/validator round-trip seeds a fresh detector with this vector
// before replaying the WAV.
struct SidecarPayload {
    std::vector<EventFeatures> events;
    std::vector<float>         noise_floor_at_first_event;
    std::uint64_t              events_total_since_boot{0};
    std::uint64_t              frames_processed_since_boot{0};
};

// EXPERIMENTAL: presets are a coarse, end-user-facing alternative to the
// fine-grained tunable manifest. Each algorithm owns the *meaning* of its
// preset names — "noisy" on a learned detector may move very different
// internal state than "noisy" on BandEnergyDetector. Names and tunings are
// expected to shift in early releases as we learn what serves field users.
struct PresetInfo {
    const char* name;
    const char* doc;
};

// Self-description of one runtime knob. Returned in bulk by ISweepTracker::
// listTunables() so callers (offline validator GUI, CLI describe) can render
// algorithm-agnostic UIs without hardcoding any algorithm's knob set.
//
// `key` and `doc` MUST point to strings with static storage duration — the
// validator's C API and Python wrapper hand these pointers across the FFI
// boundary unchanged, so they have to remain valid for the plugin's lifetime.
//
// Layout is intentionally POD and ABI-compatible with the C struct of the
// same shape in tools/validator/native/validator_c_api.h (asserted there).
struct TunableInfo {
    const char* key;
    int         type;          // 0 = float, 1 = int   (see TunableType below)
    double      default_value;
    double      min_value;
    double      max_value;
    const char* doc;
};
namespace TunableType {
    inline constexpr int Float = 0;
    inline constexpr int Int   = 1;
}

// Per-frame activity snapshot. Lets downstream consumers (e.g. the recorder)
// react on the leading edge of a detection instead of waiting for end-of-event.
//
// `active` is true while the detector considers the current frame part of a
// (possibly open-ended) hot region. `lo_hz` / `hi_hz` describe the firing
// region's current freq span when active; their values are undefined when
// active is false.
struct DetectorState {
    bool  active;
    float lo_hz;
    float hi_hz;
};

class ISweepTracker {
public:
    virtual ~ISweepTracker() = default;

    /**
     * Called once per stream session, before any processFrame() call.
     *
     * @param sampleRate  Audio sample rate in Hz.
     * @param fftSize     FFT size in samples (so magnitudes.size() == fftSize/2 + 1).
     * @param freqLoHz    Lower edge of the detection search window in Hz.
     * @param freqHiHz    Upper edge of the detection search window in Hz.
     *                    Implementations should clamp to Nyquist internally.
     */
    virtual void configure(int sampleRate, std::size_t fftSize,
                           float freqLoHz, float freqHiHz) = 0;

    /**
     * Process a single FFT magnitude frame.
     *
     * @param magnitudes      fftSize/2 + 1 magnitudes for this hop.
     * @param currentFrame    Monotonic frame index since stream start.
     * @param outState        Always populated with the per-frame activity snapshot.
     * @param outAnnotation   Populated only when the return value is true.
     * @return true iff a complete detection event just closed; outAnnotation is valid.
     *
     * MUST be real-time safe: no malloc/new, no mutex, no blocking I/O.
     */
    virtual bool processFrame(std::span<const float> magnitudes,
                              std::uint32_t currentFrame,
                              DetectorState& outState,
                              Annotation& outAnnotation) = 0;

    /**
     * Set a named tunable (e.g. "band_snr_threshold", "alpha_rise").
     *
     * Used by the offline validator to grid-search without recompiling.
     * Implementations return false for unknown keys. May be called before or
     * between processFrame() calls; effect on an in-progress event is
     * implementation-defined (typically: applies on the next frame).
     *
     * Default implementation rejects all keys, so existing trackers that have
     * no runtime knobs remain valid without changes.
     */
    virtual bool setTunable(const char* /*key*/, double /*value*/) { return false; }

    /**
     * Read a named tunable's current value into *outValue.
     *
     * Returns false (and leaves *outValue untouched) for unknown keys. The
     * offline validator uses this to query algorithm defaults so its
     * diagnostics don't carry hardcoded shadow constants.
     */
    virtual bool getTunable(const char* /*key*/, double* /*outValue*/) const { return false; }

    /**
     * Enumerate every tunable this algorithm accepts via setTunable().
     *
     * Default returns an empty span, so trackers with no runtime knobs (or
     * older plugins compiled against a prior interface) stay valid without
     * changes. Algorithms that DO have knobs should return a static
     * `constexpr` array — listTunables() is called once at UI launch and is
     * not on the audio path, but a static return keeps the .rodata footprint
     * predictable and the pointers stable across the FFI boundary.
     *
     * Implementations should make sure listTunables() lists exactly the keys
     * that setTunable()/getTunable() accept — these three functions are the
     * algorithm's public knob contract and silent drift between them would
     * defeat the point of a self-describing interface.
     */
    virtual std::span<const TunableInfo> listTunables() const { return {}; }

    /**
     * EXPERIMENTAL: apply a named coarse-grained preset (e.g. "quiet",
     * "balanced", "noisy"). Each algorithm owns the bundle of internal
     * tunable changes a preset represents — see listPresets() for the
     * names this tracker recognises.
     *
     * Returns true on success, false if `name` is not a known preset.
     * Default returns false so existing trackers that don't ship presets
     * stay valid; the caller is expected to surface this as a clear error
     * rather than silently fall through.
     *
     * Semantics: presets are a syntactic shorthand for a bundle of
     * setTunable() calls. Callers that combine a preset with explicit
     * setTunable() overrides should apply the preset first so the
     * overrides win — the validator and production app both follow that
     * order.
     */
    virtual bool applyPreset(const char* /*name*/) { return false; }

    /**
     * EXPERIMENTAL: enumerate the presets this algorithm recognises.
     * Default returns an empty span. Same lifetime rules as listTunables():
     * implementations return a static constexpr array so the string
     * pointers are valid for the plugin's lifetime.
     *
     * The preset names listed here MUST be exactly those applyPreset()
     * accepts — these two functions are the algorithm's public preset
     * contract.
     */
    virtual std::span<const PresetInfo> listPresets() const { return {}; }

    /**
     * Drain all completed event features + the noise-floor snapshot since
     * the last call into @p out. Used by the recorder to assemble a sidecar
     * JSON next to each saved WAV, so the offline validator can reproduce
     * on-device decisions without depending on the WAV being long enough
     * for its EMA floor to converge from scratch.
     *
     * Semantics:
     *  - `out.events` is filled with every event whose annotation has been
     *    emitted since the previous drain (in order).
     *  - `out.noise_floor_at_first_event` is the per-bin floor snapshotted
     *    at the moment of the FIRST event since the previous drain. Empty
     *    if no event fired in that window.
     *  - `out.events_total_since_boot` / `frames_processed_since_boot` are
     *    monotonic counters for context.
     *  - State is cleared after the drain.
     *
     * Default impl is a no-op returning false, so non-instrumented plugins
     * keep compiling and the recorder treats them as "no sidecar data
     * available" — it will just skip writing one.
     */
    virtual bool drainSidecarPayload(SidecarPayload& /*out*/) { return false; }

    /**
     * Seed the per-bin noise floor estimate. Used by the offline validator
     * to bypass the EMA's cold-start convergence period on short clips,
     * by feeding in a snapshot the device captured at trigger time.
     *
     * @param floor  Span of `fftSize/2 + 1` float values matching the
     *               algorithm's configured FFT size. Implementations that
     *               don't model a noise floor should leave the default
     *               (return false).
     * @return true if seeded, false on size mismatch or unsupported.
     */
    virtual bool seedNoiseFloor(std::span<const float> /*floor*/) { return false; }

    /**
     * Read back the algorithm's monotonic event counter (events triggered
     * since this plugin instance was constructed). Recorder logs this for
     * sidecar context; validator uses it to spot-check reproducibility.
     * Default returns 0 for plugins that don't track it.
     */
    virtual std::uint64_t totalEventsSinceBoot() const { return 0; }

    /**
     * Wait-free count of *kept* (non-gate-rejected) events since this
     * plugin instance was constructed. The recorder snapshots this on
     * beginRecording() and again at endRecording(); a clip whose window
     * saw no bat-like event is discarded on close. Detectors that don't
     * ship a false-positive gate (or don't want to participate in the
     * discard) return the same value as totalEventsSinceBoot() so every
     * clip is kept, matching the pre-gate behaviour.
     *
     * Implementations MUST make this wait-free: the recorder polls it at
     * a few-millisecond cadence and must never wait on the audio thread.
     */
    virtual std::uint64_t batLikeEventsSinceBoot() const {
        return totalEventsSinceBoot();
    }
};

// --- Plugin entry points (resolved via dlsym by TrackerRegistry) ---
extern "C" {
    typedef ISweepTracker* (*CreateTrackerFunc)();
    typedef void           (*DestroyTrackerFunc)(ISweepTracker*);
    typedef const char*    (*GetTrackerNameFunc)();
}
