// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "validator_c_api.h"

#include "app/Config.hpp"               // for the production NFFT/HOP defaults
#include "dsp/ISweepTracker.hpp"
#include "dsp/TrackerRegistry.hpp"
#include "dsp/core/IFftEngine.hpp"
#include "dsp/core/BiquadFilter.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/*
 * Layout sanity. EbDetectorState/EbAnnotation are bag-of-bytes equivalents
 * of the C++ structs so we can memcpy across the FFI boundary. If either
 * side's layout drifts, fail at compile time.
 */
static_assert(sizeof(EbDetectorState) == sizeof(DetectorState),
              "EbDetectorState layout must match DetectorState");
static_assert(sizeof(EbAnnotation)    == sizeof(Annotation),
              "EbAnnotation layout must match Annotation");
static_assert(sizeof(EbTunableInfo)   == sizeof(TunableInfo),
              "EbTunableInfo layout must match TunableInfo");
static_assert(sizeof(EbPresetInfo)    == sizeof(PresetInfo),
              "EbPresetInfo layout must match PresetInfo");
static_assert(EB_TUNABLE_TYPE_FLOAT == TunableType::Float,
              "EB_TUNABLE_TYPE_FLOAT must equal TunableType::Float");
static_assert(EB_TUNABLE_TYPE_INT   == TunableType::Int,
              "EB_TUNABLE_TYPE_INT must equal TunableType::Int");

struct EbDetector {
    std::unique_ptr<ISweepTracker, std::function<void(ISweepTracker*)>> tracker;
};

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

extern "C" size_t eb_init(const char* algorithms_dir) {
    // TrackerRegistry::scanPlugins() is idempotent on collisions, so repeated
    // calls with the same dir are safe.
    if (algorithms_dir) {
        TrackerRegistry::getInstance().scanPlugins(algorithms_dir);
    }
    return TrackerRegistry::getInstance().getAvailableAlgorithms().size();
}

extern "C" void eb_default_fft_params(size_t* fft_size, size_t* hop_size) {
    echobox::app::Config defaults;
    if (fft_size) *fft_size = defaults.fftSize;
    if (hop_size) *hop_size = defaults.hopSize;
}

// ---------------------------------------------------------------------------
// STFT
// ---------------------------------------------------------------------------

extern "C" size_t eb_stft(int sample_rate, size_t fft_size, size_t hop_size,
                          float hpf_cutoff_hz,
                          const float* samples, size_t n_samples,
                          float* mags_out, size_t mags_capacity) {
    if (!samples || !mags_out || hop_size == 0 || fft_size == 0) return 0;
    if (n_samples < hop_size) return 0;

    const size_t bins   = fft_size / 2 + 1;
    const size_t frames = n_samples / hop_size;
    if (frames * bins > mags_capacity) return 0;

    auto fft = echobox::dsp::makeFftEngine(hop_size, fft_size);

    // Apply HPF (if any) into a scratch buffer so we never mutate caller memory.
    std::vector<float> filtered(samples, samples + n_samples);
    if (hpf_cutoff_hz > 0.0f) {
        BiquadFilter hpf;
        hpf.configureHighPass(hpf_cutoff_hz, static_cast<float>(sample_rate));
        for (auto& s : filtered) s = hpf.process(s);
    }

    std::vector<float> mag(bins, 0.0f);
    for (size_t f = 0; f < frames; ++f) {
        const std::span<const float> in(filtered.data() + f * hop_size, hop_size);
        fft->process(in, mag);
        std::memcpy(mags_out + f * bins, mag.data(), bins * sizeof(float));
    }
    return frames;
}

// ---------------------------------------------------------------------------
// Detector
// ---------------------------------------------------------------------------

extern "C" EbDetector* eb_detector_create(const char* algorithm_name,
                                          int sample_rate, size_t fft_size,
                                          float freq_lo_hz, float freq_hi_hz) {
    if (!algorithm_name) return nullptr;
    auto t = TrackerRegistry::getInstance().createTracker(algorithm_name);
    if (!t) return nullptr;
    t->configure(sample_rate, fft_size, freq_lo_hz, freq_hi_hz);
    return new EbDetector{ std::move(t) };
}

extern "C" void eb_detector_destroy(EbDetector* d) {
    delete d;
}

extern "C" bool eb_detector_process_frame(EbDetector* d,
                                          const float* mags, size_t n_bins,
                                          uint32_t current_frame,
                                          EbDetectorState* out_state,
                                          EbAnnotation* out_ann) {
    if (!d || !mags || !out_state || !out_ann) return false;

    DetectorState s{};
    Annotation    a{};
    const bool emitted = d->tracker->processFrame(
        std::span<const float>(mags, n_bins), current_frame, s, a);

    std::memcpy(out_state, &s, sizeof(s));
    if (emitted) std::memcpy(out_ann, &a, sizeof(a));
    return emitted;
}

extern "C" bool eb_detector_set_tunable(EbDetector* d, const char* key, double value) {
    if (!d || !key) return false;
    return d->tracker->setTunable(key, value);
}

extern "C" bool eb_detector_get_tunable(EbDetector* d, const char* key, double* out_value) {
    if (!d || !key || !out_value) return false;
    return d->tracker->getTunable(key, out_value);
}

extern "C" size_t eb_detector_list_tunables(EbDetector* d, EbTunableInfo* out_infos, size_t max) {
    if (!d) return 0;
    const auto src = d->tracker->listTunables();
    if (out_infos) {
        const size_t n = std::min(src.size(), max);
        // Layout-compatible per the static_asserts at top of file, so a single
        // memcpy preserves every field including the string pointers.
        std::memcpy(out_infos, src.data(), n * sizeof(EbTunableInfo));
    }
    return src.size();
}

extern "C" bool eb_detector_apply_preset(EbDetector* d, const char* name) {
    if (!d || !name) return false;
    return d->tracker->applyPreset(name);
}

extern "C" size_t eb_detector_list_presets(EbDetector* d, EbPresetInfo* out_infos, size_t max) {
    if (!d) return 0;
    const auto src = d->tracker->listPresets();
    if (out_infos) {
        const size_t n = std::min(src.size(), max);
        std::memcpy(out_infos, src.data(), n * sizeof(EbPresetInfo));
    }
    return src.size();
}

extern "C" size_t eb_list_algorithms(const char** out_names, size_t max) {
    // Stash the names in a process-lifetime cache so the returned pointers
    // stay valid for the caller (vector<string> would dangle on resize).
    static std::mutex mu;
    static std::vector<std::string> cache;

    std::lock_guard<std::mutex> lk(mu);
    cache = TrackerRegistry::getInstance().getAvailableAlgorithms();
    const size_t n = std::min(cache.size(), max);
    if (out_names) {
        for (size_t i = 0; i < n; ++i) out_names[i] = cache[i].c_str();
    }
    return cache.size();
}
