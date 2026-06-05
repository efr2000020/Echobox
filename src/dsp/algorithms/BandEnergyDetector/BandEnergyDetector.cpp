// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// BandEnergyDetector implementation. See BandEnergyDetector.hpp for the
/// signal-processing rationale; this TU also defines the @c extern "C"
/// plugin entry points at the bottom.

#include "BandEnergyDetector.hpp"
#include "logging/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

constexpr float BandEnergyDetector::BAND_EDGES_HZ[];

BandEnergyDetector::BandEnergyDetector()
    : m_sampleRate(0), m_fftSize(0), m_binResolution(0.0f),
      m_freqLoHz(0.0f), m_freqHiHz(0.0f),
      m_inBandLo(0), m_inBandHi(0),
      m_floorSeeded(false), m_warmupFrames(0),
      m_inEvent(false), m_activeRun(0), m_silenceFrames(0),
      m_eventStart(0), m_eventLoHz(0.0f), m_eventHiHz(0.0f),
      m_eventPeakSnr(0.0f),
      m_frameCount(0), m_maxBandSnrInHeartbeat(0.0f), m_peakBandInHeartbeat(-1) {}

void BandEnergyDetector::configure(int sampleRate, std::size_t fftSize,
                                   float freqLoHz, float freqHiHz) {
    m_sampleRate    = sampleRate;
    m_fftSize       = fftSize;
    m_binResolution = static_cast<float>(sampleRate) / static_cast<float>(fftSize);
    m_freqLoHz      = freqLoHz;
    m_freqHiHz      = freqHiHz;

    const std::size_t numBins = fftSize / 2 + 1;
    const float nyquist = sampleRate / 2.0f;
    const float userLo = std::max(0.0f, freqLoHz);
    const float userHi = std::min(nyquist, freqHiHz);

    // Build sub-bands from the fixed Hz edges, intersected with the user's
    // [freqLoHz, freqHiHz] window. Bands entirely outside the window are dropped.
    m_bands.clear();
    for (int i = 0; i < MAX_BANDS; ++i) {
        const float bandLoHz = std::max(BAND_EDGES_HZ[i],     userLo);
        const float bandHiHz = std::min(BAND_EDGES_HZ[i + 1], userHi);
        if (bandHiHz <= bandLoHz) continue;

        std::size_t lo = static_cast<std::size_t>(bandLoHz / m_binResolution);
        std::size_t hi = static_cast<std::size_t>(bandHiHz / m_binResolution);
        if (hi > numBins) hi = numBins;
        if (lo >= numBins) break;
        if (hi > lo + 1) {
            m_bands.push_back({lo, hi});
        }
    }

    m_noiseFloor.assign(numBins, 0.0f);
    m_floorSeeded   = false;
    m_warmupFrames  = 0;
    m_bandSnrScratch.assign(numBins, 0.0f);

    if (!m_bands.empty()) {
        m_inBandLo = m_bands.front().loBin;
        m_inBandHi = m_bands.back().hiBin;
    } else {
        m_inBandLo = 0;
        m_inBandHi = 0;
    }

    m_inEvent               = false;
    m_activeRun             = 0;
    m_silenceFrames         = 0;
    m_eventPeakSnr          = 0.0f;
    m_frameCount            = 0;
    m_maxBandSnrInHeartbeat = 0.0f;
    m_peakBandInHeartbeat   = -1;

    // Reset sidecar diagnostic state — configure() may be called on a fresh
    // stream, and we don't want stale pending events bleeding through.
    {
        std::lock_guard<std::mutex> lk(m_diagnosticsMutex);
        m_pendingEvents.clear();
        m_floorSnapshotAtFirstEvent.clear();
        m_inProgressEvent          = EventFeatures{};
        m_framesProcessedSinceBoot = 0;
        // m_eventsTotalSinceBoot is a lifetime counter; keep across reconfigures.
    }

    LS_INFO("dsp.bed", "configured: sr=%d fft=%zu res=%.1fHz bands=%zu window=%.0f-%.0fHz",
            sampleRate, fftSize, m_binResolution, m_bands.size(), userLo, userHi);
}

bool BandEnergyDetector::processFrame(std::span<const float> magnitudes,
                                      std::uint32_t currentFrame,
                                      DetectorState& outState,
                                      Annotation& outAnnotation) {
    ++m_frameCount;
    ++m_warmupFrames;
    ++m_framesProcessedSinceBoot;

    if (m_noiseFloor.size() != magnitudes.size()) {
        m_noiseFloor.assign(magnitudes.size(), 0.0f);
        m_bandSnrScratch.assign(magnitudes.size(), 0.0f);
        m_floorSeeded = false;
    }

    if (!m_floorSeeded) {
        for (std::size_t i = 0; i < magnitudes.size(); ++i) {
            m_noiseFloor[i] = magnitudes[i];
        }
        m_floorSeeded = true;
    }

    // 1. Per-band detection statistic: mean SNR of the top-K bins in each band.
    float bestBandSnr = 0.0f;
    int   bestBand    = -1;

    for (std::size_t bi = 0; bi < m_bands.size(); ++bi) {
        const std::size_t lo = m_bands[bi].loBin;
        const std::size_t hi = m_bands[bi].hiBin;

        const std::size_t count = hi - lo;
        for (std::size_t i = lo; i < hi; ++i) {
            const float floor = (m_noiseFloor[i] > m_minAbsFloor) ? m_noiseFloor[i] : m_minAbsFloor;
            m_bandSnrScratch[i] = magnitudes[i] / floor;
        }

        const int k = static_cast<int>(std::min(static_cast<std::size_t>(m_topK), count));
        if (k <= 0) continue;

        float* begin = m_bandSnrScratch.data() + lo;
        float* end   = m_bandSnrScratch.data() + hi;
        std::nth_element(begin, end - k, end);
        float sum = 0.0f;
        for (float* p = end - k; p != end; ++p) sum += *p;
        const float bandSnr = sum / static_cast<float>(k);

        if (bandSnr > bestBandSnr) {
            bestBandSnr = bandSnr;
            bestBand    = static_cast<int>(bi);
        }
    }

    // 2. Update the per-bin EMA floor (asymmetric: slow rise, fast fall).
    for (std::size_t i = 0; i < magnitudes.size(); ++i) {
        const float mag   = magnitudes[i];
        const float floor = m_noiseFloor[i];
        const float alpha = (mag > floor) ? m_alphaRise : m_alphaFall;
        m_noiseFloor[i] = alpha * floor + (1.0f - alpha) * mag;
    }

    const bool warmedUp = (m_warmupFrames >= m_warmupFramesLimit);

    // 3. Spectral-flatness gate (false-positive defense vs broadband clicks etc).
    float flatness = 1.0f;
    if (m_inBandHi > m_inBandLo) {
        double logSum = 0.0;
        double linSum = 0.0;
        const std::size_t n = m_inBandHi - m_inBandLo;
        for (std::size_t i = m_inBandLo; i < m_inBandHi; ++i) {
            const double v = static_cast<double>(magnitudes[i]) + 1e-12;
            logSum += std::log(v);
            linSum += v;
        }
        const double geoMean = std::exp(logSum / static_cast<double>(n));
        const double ariMean = linSum / static_cast<double>(n);
        flatness = static_cast<float>(geoMean / (ariMean + 1e-12));
    }
    const bool flatnessOk = (flatness > m_minFlatness) && (flatness < m_maxFlatness);

    const bool hot = warmedUp && (bestBand >= 0)
                     && (bestBandSnr > m_bandSnrThreshold)
                     && flatnessOk;

    // Diagnostic heartbeat (debug-level, dropped before enqueue if minLevel>Debug).
    if (bestBandSnr > m_maxBandSnrInHeartbeat) {
        m_maxBandSnrInHeartbeat = bestBandSnr;
        m_peakBandInHeartbeat   = bestBand;
    }
    if (m_frameCount % 100 == 0) {
        if (m_peakBandInHeartbeat >= 0 && m_peakBandInHeartbeat < static_cast<int>(m_bands.size())) {
            LS_DEBUG("dsp.bed", "hb frame=%u maxSnr=%.2f band=%d-%dkHz",
                     currentFrame, m_maxBandSnrInHeartbeat,
                     static_cast<int>(m_bands[m_peakBandInHeartbeat].loBin * m_binResolution / 1000),
                     static_cast<int>(m_bands[m_peakBandInHeartbeat].hiBin * m_binResolution / 1000));
        } else {
            LS_DEBUG("dsp.bed", "hb frame=%u maxSnr=%.2f", currentFrame, m_maxBandSnrInHeartbeat);
        }
        m_maxBandSnrInHeartbeat = 0.0f;
        m_peakBandInHeartbeat   = -1;
    }

    // 4. Temporal state machine: debounce in, hangover out.
    bool emittedAnnotation = false;
    if (hot) {
        const float bandLoHz = m_bands[bestBand].loBin * m_binResolution;
        const float bandHiHz = m_bands[bestBand].hiBin * m_binResolution;

        ++m_activeRun;
        m_silenceFrames = 0;

        if (!m_inEvent && m_activeRun >= m_minActiveFrames) {
            m_inEvent      = true;
            m_eventStart   = currentFrame - static_cast<std::uint32_t>(m_activeRun - 1);
            m_eventLoHz    = bandLoHz;
            m_eventHiHz    = bandHiHz;
            m_eventPeakSnr = bestBandSnr;
            // Flatness + band index at trigger time are the two things you'd
            // want to read off a log line when a recording later turns out
            // to be a false positive — they tell you which gate let it past.
            LS_INFO("dsp.bed", "event start frame=%u snr=%.2f band=%d flatness=%.3f",
                    m_eventStart, bestBandSnr, bestBand + 1, flatness);

            // Stage this event's diagnostics for the recorder/sidecar. If
            // this is the first event since the last drain, also snapshot
            // the per-bin noise floor — the offline validator seeds from it
            // to reproduce on-device decisions on recordings too short for
            // cold-start convergence.
            {
                std::lock_guard<std::mutex> lk(m_diagnosticsMutex);
                m_inProgressEvent = EventFeatures{};
                m_inProgressEvent.start_frame      = m_eventStart;
                m_inProgressEvent.band_index       = static_cast<std::int16_t>(bestBand + 1);
                m_inProgressEvent.trigger_snr      = bestBandSnr;
                m_inProgressEvent.trigger_flatness = flatness;
                m_inProgressEvent.peak_snr         = bestBandSnr;
                m_inProgressEvent.lo_hz            = bandLoHz;
                m_inProgressEvent.hi_hz            = bandHiHz;
                if (m_floorSnapshotAtFirstEvent.empty() && m_pendingEvents.empty()) {
                    m_floorSnapshotAtFirstEvent.assign(
                        m_noiseFloor.begin(), m_noiseFloor.end());
                }
                ++m_eventsTotalSinceBoot;
            }
        }
        if (m_inEvent) {
            m_eventLoHz    = std::min(m_eventLoHz, bandLoHz);
            m_eventHiHz    = std::max(m_eventHiHz, bandHiHz);
            m_eventPeakSnr = std::max(m_eventPeakSnr, bestBandSnr);
            // Keep the in-progress event's union span + peak SNR in sync so
            // an early drain (e.g. recorder closes the WAV before this event
            // closes) sees consistent intermediate values.
            std::lock_guard<std::mutex> lk(m_diagnosticsMutex);
            m_inProgressEvent.lo_hz    = std::min(m_inProgressEvent.lo_hz, bandLoHz);
            m_inProgressEvent.hi_hz    = std::max(m_inProgressEvent.hi_hz, bandHiHz);
            m_inProgressEvent.peak_snr = std::max(m_inProgressEvent.peak_snr, bestBandSnr);
        }
    } else {
        if (m_inEvent) {
            ++m_silenceFrames;
            if (m_silenceFrames > m_hangoverFrames) {
                outAnnotation.start_frame = m_eventStart;
                outAnnotation.end_frame   = currentFrame - static_cast<std::uint32_t>(m_silenceFrames);
                outAnnotation.low_freq    = m_eventLoHz;
                outAnnotation.high_freq   = m_eventHiHz;

                LS_INFO("dsp.bed", "event end frames=%u-%u %.1f-%.1fkHz peakSnr=%.2f",
                        m_eventStart, outAnnotation.end_frame,
                        m_eventLoHz / 1000.0f, m_eventHiHz / 1000.0f,
                        m_eventPeakSnr);

                // Finalise and stash this event's diagnostics for the next
                // drainSidecarPayload() call.
                {
                    std::lock_guard<std::mutex> lk(m_diagnosticsMutex);
                    m_inProgressEvent.end_frame       = outAnnotation.end_frame;
                    m_inProgressEvent.duration_frames = static_cast<std::uint16_t>(
                        std::min<std::uint32_t>(
                            outAnnotation.end_frame - m_eventStart + 1u,
                            std::numeric_limits<std::uint16_t>::max()));
                    m_inProgressEvent.lo_hz    = m_eventLoHz;
                    m_inProgressEvent.hi_hz    = m_eventHiHz;
                    m_inProgressEvent.peak_snr = m_eventPeakSnr;
                    m_pendingEvents.push_back(m_inProgressEvent);
                }

                m_inEvent          = false;
                m_activeRun        = 0;
                m_silenceFrames    = 0;
                m_eventPeakSnr     = 0.0f;
                emittedAnnotation  = true;
            }
        } else {
            m_activeRun = 0;
        }
    }

    // 5. Per-frame state snapshot for the downstream recorder.
    outState.active = m_inEvent;
    outState.lo_hz  = m_inEvent ? m_eventLoHz : 0.0f;
    outState.hi_hz  = m_inEvent ? m_eventHiHz : 0.0f;

    return emittedAnnotation;
}

// Keep setTunable / getTunable / listTunables in lock-step. Every new tunable
// needs an entry in all three — with ~10 knobs and a setTunable() that rejects
// unknown keys the drift surface is small, but listTunables() in particular
// is the source the GUI form is rendered from, so an omission silently hides
// a knob rather than failing loudly.

bool BandEnergyDetector::setTunable(const char* key, double value) {
    if (!key) return false;
    if (std::strcmp(key, "alpha_rise")         == 0) { m_alphaRise         = static_cast<float>(value); return true; }
    if (std::strcmp(key, "alpha_fall")         == 0) { m_alphaFall         = static_cast<float>(value); return true; }
    if (std::strcmp(key, "min_abs_floor")      == 0) { m_minAbsFloor       = static_cast<float>(value); return true; }
    if (std::strcmp(key, "band_snr_threshold") == 0) { m_bandSnrThreshold  = static_cast<float>(value); return true; }
    if (std::strcmp(key, "min_flatness")       == 0) { m_minFlatness       = static_cast<float>(value); return true; }
    if (std::strcmp(key, "max_flatness")       == 0) { m_maxFlatness       = static_cast<float>(value); return true; }
    if (std::strcmp(key, "top_k")              == 0) { m_topK              = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "warmup_frames")      == 0) { m_warmupFramesLimit = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "min_active_frames")  == 0) { m_minActiveFrames   = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "hangover_frames")    == 0) { m_hangoverFrames    = static_cast<int>(value);   return true; }
    return false;
}

bool BandEnergyDetector::getTunable(const char* key, double* outValue) const {
    if (!key || !outValue) return false;
    if (std::strcmp(key, "alpha_rise")         == 0) { *outValue = m_alphaRise;         return true; }
    if (std::strcmp(key, "alpha_fall")         == 0) { *outValue = m_alphaFall;         return true; }
    if (std::strcmp(key, "min_abs_floor")      == 0) { *outValue = m_minAbsFloor;       return true; }
    if (std::strcmp(key, "band_snr_threshold") == 0) { *outValue = m_bandSnrThreshold;  return true; }
    if (std::strcmp(key, "min_flatness")       == 0) { *outValue = m_minFlatness;       return true; }
    if (std::strcmp(key, "max_flatness")       == 0) { *outValue = m_maxFlatness;       return true; }
    if (std::strcmp(key, "top_k")              == 0) { *outValue = m_topK;              return true; }
    if (std::strcmp(key, "warmup_frames")      == 0) { *outValue = m_warmupFramesLimit; return true; }
    if (std::strcmp(key, "min_active_frames")  == 0) { *outValue = m_minActiveFrames;   return true; }
    if (std::strcmp(key, "hangover_frames")    == 0) { *outValue = m_hangoverFrames;    return true; }
    return false;
}

std::span<const TunableInfo> BandEnergyDetector::listTunables() const {
    // Static so the returned pointers stay valid for the plugin's lifetime —
    // the validator's C API and Python wrapper pass these strings across the
    // FFI boundary unchanged. Lives in .rodata; zero per-call cost.
    static constexpr TunableInfo kTunables[] = {
        {"band_snr_threshold", TunableType::Float, 12.0,  0.0,    200.0,
         "Top-K mean band SNR above which a frame counts as hot."},
        {"min_flatness",       TunableType::Float, 0.10,  0.0,    1.0,
         "Spectral-flatness lower bound (rejects pure tones)."},
        {"max_flatness",       TunableType::Float, 0.65,  0.0,    1.0,
         "Spectral-flatness upper bound (rejects broadband noise)."},
        {"top_k",              TunableType::Int,   8.0,   1.0,    64.0,
         "Number of brightest bins per band averaged for the SNR statistic."},
        {"min_active_frames",  TunableType::Int,   2.0,   1.0,    100.0,
         "Consecutive hot frames required before an event opens (debounce)."},
        {"hangover_frames",    TunableType::Int,   8.0,   1.0,    200.0,
         "Consecutive quiet frames tolerated inside an event before it closes."},
        {"warmup_frames",      TunableType::Int,   40.0,  0.0,    1000.0,
         "Frames discarded after start while the noise-floor EMA seeds."},
        {"alpha_rise",         TunableType::Float, 0.995, 0.0,    1.0,
         "EMA coefficient when the floor is rising (slow)."},
        {"alpha_fall",         TunableType::Float, 0.90,  0.0,    1.0,
         "EMA coefficient when the floor is falling (fast)."},
        {"min_abs_floor",      TunableType::Float, 1e-6,  0.0,    1.0,
         "Absolute lower bound on the noise floor (prevents divide-by-zero spikes)."},
    };
    return std::span<const TunableInfo>(kTunables, sizeof(kTunables) / sizeof(kTunables[0]));
}

// --- EXPERIMENTAL: sensitivity presets ------------------------------------
//
// Each preset is a bundle of (key, value) overrides applied via setTunable.
// The three names are the same across the project (CLI, GUI, README) so the
// experience is consistent; the *internal* tunings here are subject to
// revision as we collect more field data. Adding a new preset means writing
// a new entry in kPresetBundles and adding its descriptor to kPresets.
//
// Numbers below are starting points sized for typical UltraMic384K
// deployments. They should be re-validated against the offline grid-search
// workflow as the BandEnergyDetector evolves.

namespace {
struct PresetEntry {
    const char* key;
    double      value;
};

constexpr PresetEntry kQuietBundle[] = {
    {"band_snr_threshold",  8.0},
    {"max_flatness",        0.80},
    {"min_active_frames",   1.0},
};
constexpr PresetEntry kBalancedBundle[] = {
    {"band_snr_threshold", 12.0},
    {"max_flatness",        0.65},
    {"min_active_frames",   2.0},
};
constexpr PresetEntry kNoisyBundle[] = {
    {"band_snr_threshold", 18.0},
    {"max_flatness",        0.60},
    {"min_active_frames",   3.0},
};

constexpr PresetInfo kPresets[] = {
    {"quiet",
     "(experimental) Low-noise site, maximum recall. Catches faint distant calls at the cost of more false positives."},
    {"balanced",
     "(experimental) Echobox defaults. Suitable for typical unattended deployments."},
    {"noisy",
     "(experimental) Windy, suburban, or near-roadway sites. Strict on every dimension; only loud unambiguous calls survive."},
};
} // namespace

bool BandEnergyDetector::applyPreset(const char* name) {
    if (!name) return false;
    std::span<const PresetEntry> bundle;
    if      (std::strcmp(name, "quiet")    == 0) bundle = kQuietBundle;
    else if (std::strcmp(name, "balanced") == 0) bundle = kBalancedBundle;
    else if (std::strcmp(name, "noisy")    == 0) bundle = kNoisyBundle;
    else return false;

    for (const auto& e : bundle) {
        // setTunable rejects unknown keys; if it ever does, the preset table
        // is out of sync with the algorithm's knob set — bail rather than
        // silently apply a partial preset.
        if (!setTunable(e.key, e.value)) return false;
    }
    LS_INFO("dsp.bed", "applied preset '%s' (%zu tunables)", name, bundle.size());
    return true;
}

std::span<const PresetInfo> BandEnergyDetector::listPresets() const {
    return std::span<const PresetInfo>(kPresets,
                                       sizeof(kPresets) / sizeof(kPresets[0]));
}

// --- Sidecar diagnostics --------------------------------------------------
//
// drain* / seed* / counter accessors used by the Recorder (and the validator's
// replay path through the C API). All synchronization happens under one
// mutex; contention is effectively nil because drain runs at WAV-close cadence
// and the producer only touches the mutex on event-open / event-end.

bool BandEnergyDetector::drainSidecarPayload(SidecarPayload& out) {
    std::lock_guard<std::mutex> lk(m_diagnosticsMutex);
    out.events.assign(m_pendingEvents.begin(), m_pendingEvents.end());
    out.noise_floor_at_first_event.assign(m_floorSnapshotAtFirstEvent.begin(),
                                          m_floorSnapshotAtFirstEvent.end());
    out.events_total_since_boot        = m_eventsTotalSinceBoot;
    out.frames_processed_since_boot    = m_framesProcessedSinceBoot;
    m_pendingEvents.clear();
    m_floorSnapshotAtFirstEvent.clear();
    return true;
}

bool BandEnergyDetector::seedNoiseFloor(std::span<const float> floor) {
    if (floor.empty()) return false;
    // The validator constructs a detector with the same fft_size the device
    // captured under, so the bin counts must match. Mismatch is a hard error
    // rather than a silent partial copy — replay results would be wrong.
    if (m_noiseFloor.size() != floor.size()) return false;
    for (std::size_t i = 0; i < floor.size(); ++i) {
        m_noiseFloor[i] = floor[i];
    }
    m_floorSeeded = true;
    // Skip the EMA warmup gate too — the seed is precisely the converged
    // state we'd otherwise be waiting for.
    m_warmupFrames = m_warmupFramesLimit;
    return true;
}

std::uint64_t BandEnergyDetector::totalEventsSinceBoot() const {
    std::lock_guard<std::mutex> lk(m_diagnosticsMutex);
    return m_eventsTotalSinceBoot;
}

// --- Plugin entry points (resolved by TrackerRegistry via dlsym) ---
extern "C" {
    ISweepTracker* create_tracker()                    { return new BandEnergyDetector(); }
    void           destroy_tracker(ISweepTracker* t)   { delete static_cast<BandEnergyDetector*>(t); }
    const char*    get_tracker_name()                  { return "BandEnergyDetector"; }
}
