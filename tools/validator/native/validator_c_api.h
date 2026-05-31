#pragma once
/*
 * C ABI used by the Python validator (tools/validator/native.py).
 *
 * Wraps the same DSP code the production binary ships: the KissFFT engine,
 * the HPF biquad, and ISweepTracker plugins loaded from a configurable
 * algorithms directory. The Python side loads libechobox_validator.so via
 * ctypes and drives this surface directly — no shadow detector port.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Layout-compatible with echobox C++ DetectorState. */
typedef struct {
    bool  active;
    float lo_hz;
    float hi_hz;
} EbDetectorState;

/* Layout-compatible with the packed C++ Annotation struct. */
#pragma pack(push, 1)
typedef struct {
    uint32_t start_frame;
    uint32_t end_frame;
    float    high_freq;
    float    low_freq;
} EbAnnotation;
#pragma pack(pop)

/* Layout-compatible with the C++ TunableInfo struct in dsp/ISweepTracker.hpp.
 * `key` and `doc` are owned by the plugin and remain valid for the process
 * lifetime, so the caller may hold the pointers without copying. */
#define EB_TUNABLE_TYPE_FLOAT 0
#define EB_TUNABLE_TYPE_INT   1
typedef struct {
    const char* key;
    int         type;            /* one of EB_TUNABLE_TYPE_* */
    double      default_value;
    double      min_value;
    double      max_value;
    const char* doc;
} EbTunableInfo;

/* EXPERIMENTAL. Layout-compatible with the C++ PresetInfo struct in
 * dsp/ISweepTracker.hpp. `name` and `doc` point to plugin-owned storage
 * valid for the process lifetime. */
typedef struct {
    const char* name;
    const char* doc;
} EbPresetInfo;

/* --- Initialization ------------------------------------------------------
 *
 * Scan algorithms_dir for ISweepTracker plugin .so files and register them.
 * Idempotent: calling more than once is harmless. Returns the number of
 * algorithms successfully loaded after the scan (cumulative, not just from
 * this call). Pass NULL to skip the scan and just report the count.
 */
size_t eb_init(const char* algorithms_dir);

/* --- Production defaults --------------------------------------------------
 *
 * Returns the production runtime defaults (sourced from src/app/Config.hpp)
 * so the Python validator doesn't carry hardcoded shadow constants. Either
 * pointer may be NULL.
 */
void eb_default_fft_params(size_t* fft_size, size_t* hop_size);

/* --- Stateless STFT (matches the production DspPipeline exactly) ----------
 *
 * Applies an HPF biquad (if hpf_cutoff_hz > 0), then pumps samples through
 * KissFftEngine hop_size-at-a-time. Frame count = n_samples / hop_size; the
 * first (fft_size/hop_size - 1) frames have zero-padded history, identical
 * to the production runtime.
 *
 * mags_out is row-major: index = frame * (fft_size/2 + 1) + bin.
 * Returns the number of frames written, or 0 if mags_capacity is too small.
 */
size_t eb_stft(int sample_rate, size_t fft_size, size_t hop_size,
               float hpf_cutoff_hz,
               const float* samples, size_t n_samples,
               float* mags_out, size_t mags_capacity);

/* --- Frame-level detector ------------------------------------------------- */
typedef struct EbDetector EbDetector;

/* Returns NULL if algorithm_name isn't registered (call eb_init first). */
EbDetector* eb_detector_create(const char* algorithm_name,
                               int sample_rate, size_t fft_size,
                               float freq_lo_hz, float freq_hi_hz);
void        eb_detector_destroy(EbDetector*);

/* Returns true iff a complete event closed on this frame (out_ann populated).
 * out_state is always populated. */
bool eb_detector_process_frame(EbDetector*,
                               const float* mags, size_t n_bins,
                               uint32_t current_frame,
                               EbDetectorState* out_state,
                               EbAnnotation*    out_ann);

/* Set / read a named tunable (forwarded to ISweepTracker).
 * Both return false for unknown keys. */
bool eb_detector_set_tunable(EbDetector*, const char* key, double value);
bool eb_detector_get_tunable(EbDetector*, const char* key, double* out_value);

/* Enumerate every tunable this detector accepts. Writes up to `max` entries
 * into out_infos and returns the total available (may exceed `max`). String
 * fields inside the entries point into plugin-owned storage and stay valid
 * for the detector's lifetime — no copy needed. */
size_t eb_detector_list_tunables(EbDetector*, EbTunableInfo* out_infos, size_t max);

/* EXPERIMENTAL. Apply a named coarse-grained preset (e.g. "quiet" / "noisy").
 * Returns false if `name` is not a recognised preset for this algorithm.
 * Callers combining a preset with explicit set_tunable() overrides should
 * apply the preset first so the overrides win. */
bool eb_detector_apply_preset(EbDetector*, const char* name);

/* EXPERIMENTAL. Enumerate the presets this algorithm recognises. Same
 * storage rules as eb_detector_list_tunables. */
size_t eb_detector_list_presets(EbDetector*, EbPresetInfo* out_infos, size_t max);

/* Lists registered algorithm names. Writes up to `max` C strings (owned by
 * the library, valid for the process lifetime) into out_names; returns the
 * total number registered (may exceed `max`). */
size_t eb_list_algorithms(const char** out_names, size_t max);

#ifdef __cplusplus
}
#endif
