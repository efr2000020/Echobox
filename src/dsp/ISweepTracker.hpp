#pragma once
#include <span>
#include <cstddef>
#include <cstdint>

// Stable, pre-existing detection event record. Emitted when a hot region closes.
#pragma pack(push, 1)
struct Annotation {
    uint32_t start_frame;
    uint32_t end_frame;
    float high_freq;
    float low_freq;
};
#pragma pack(pop)

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
};

// --- Plugin entry points (resolved via dlsym by TrackerRegistry) ---
extern "C" {
    typedef ISweepTracker* (*CreateTrackerFunc)();
    typedef void           (*DestroyTrackerFunc)(ISweepTracker*);
    typedef const char*    (*GetTrackerNameFunc)();
}
