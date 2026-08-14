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
      m_frameCount(0), m_maxBandSnrInHeartbeat(0.0f), m_peakBandInHeartbeat(-1),
      m_domBinRing{}, m_domMagRing{},
      m_domRingCount(0), m_domRingHead(0),
      m_dominantFramePeakMag(0.0f),
      m_dominantAnchorBin(0),
      m_dominantBandLoBin(0),
      m_dominantBandHiBin(0) {}

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

    // --- Sweep-shape ring + dominant-frame snapshot ---
    // Pre-allocate the dominant-frame magnitude snapshot at the configured
    // FFT bin count. Never resized later — processFrame()'s std::copy into
    // it is the only writer, and the size is fixed from here on.
    m_dominantFrameMags.assign(numBins, 0.0f);
    m_dominantFramePeakMag  = 0.0f;
    m_dominantAnchorBin     = 0;
    m_dominantBandLoBin     = 0;
    m_dominantBandHiBin     = 0;
    m_domRingCount          = 0;
    m_domRingHead           = 0;
    m_provisionalSuppressed          = false;
    m_gateRejected                   = false;
    m_provisionalRejectedThisEvent   = false;
    m_framesSinceLastProvisionalEval = 0;

    // Reset the cross-event onset ring. The ring intentionally survives an
    // event close (its whole point is measuring inter-event regularity), so
    // configure() is the only place it clears.
    m_onsetCount = 0;
    m_onsetHead  = 0;

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
    // While we're already walking each band, also remember the loudest *raw
    // magnitude* bin within it — when this band wins, that bin becomes the
    // event's per-frame dominant bin (used by the sweep-shape gate).
    float       bestBandSnr     = 0.0f;
    int         bestBand        = -1;
    std::size_t bestPeakBin     = 0;
    float       bestPeakBinMag  = 0.0f;

    for (std::size_t bi = 0; bi < m_bands.size(); ++bi) {
        const std::size_t lo = m_bands[bi].loBin;
        const std::size_t hi = m_bands[bi].hiBin;

        const std::size_t count = hi - lo;
        std::size_t bandPeakBin = lo;
        float       bandPeakMag = 0.0f;
        for (std::size_t i = lo; i < hi; ++i) {
            const float floor = (m_noiseFloor[i] > m_minAbsFloor) ? m_noiseFloor[i] : m_minAbsFloor;
            m_bandSnrScratch[i] = magnitudes[i] / floor;
            if (magnitudes[i] > bandPeakMag) {
                bandPeakMag = magnitudes[i];
                bandPeakBin = i;
            }
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
            bestBandSnr    = bandSnr;
            bestBand       = static_cast<int>(bi);
            bestPeakBin    = bandPeakBin;
            bestPeakBinMag = bandPeakMag;
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
            // Fresh sweep-shape ring + per-event gate state. We don't carry
            // the activeRun-1 pre-debounce frames into the ring; they were
            // sub-threshold and not part of the call we want to characterise.
            m_domRingCount         = 0;
            m_domRingHead          = 0;
            m_dominantFramePeakMag = 0.0f;
            m_dominantAnchorBin    = 0;
            m_dominantBandLoBin    = 0;
            m_dominantBandHiBin    = 0;
            m_provisionalSuppressed          = false;
            m_gateRejected                   = false;
            m_provisionalRejectedThisEvent   = false;
            m_framesSinceLastProvisionalEval = 0;
            // Record the onset in the cross-event ring for the temporal
            // repetition-rate guard. Circular append — no allocation, no
            // lock; RT-safe. The ring persists across events (it lives past
            // event-close) so the guard can measure inter-onset regularity.
            m_onsetFrames[m_onsetHead] = m_eventStart;
            m_onsetHead = (m_onsetHead + 1) % ONSET_RING_CAP;
            if (m_onsetCount < ONSET_RING_CAP) ++m_onsetCount;
            // Per-event trace: DEBUG so the default operational log doesn't
            // get one line per detection. Field operators watch the
            // HEARTBEAT counters + the saved WAVs + their sidecars;
            // --log-level debug re-enables the per-event trace for offline
            // diagnosis, where flatness + band index at trigger time are
            // the two things you'd want to read when a recording turns
            // out to be a false positive (they tell you which gate let it past).
            LS_DEBUG("dsp.bed", "event start frame=%u snr=%.2f band=%d flatness=%.3f",
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
            // m_eventLoHz/Hi/PeakSnr are the lock-free per-event scratch:
            // the audio thread owns them exclusively and the sidecar
            // snapshot at event close (below, under m_diagnosticsMutex)
            // reads them out into m_inProgressEvent in one go. Taking a
            // mutex per frame just to keep m_inProgressEvent in sync would
            // be a per-frame RT hazard; callers of drainSidecarPayload()
            // see the in-progress event only once it closes anyway.
            m_eventLoHz    = std::min(m_eventLoHz, bandLoHz);
            m_eventHiHz    = std::max(m_eventHiHz, bandHiHz);
            m_eventPeakSnr = std::max(m_eventPeakSnr, bestBandSnr);
            // --- Sweep-shape feature tracking ---
            // Record this frame's dominant in-band bin in the ring, and
            // snapshot the full magnitude spectrum if this is the loudest
            // dominant frame we've seen so far (used for the 10-dB bandwidth
            // walk at gate-decision / event-close time). std::copy of ~2049
            // floats is ~8 KB and fires only when a new max appears within
            // an open event; well within the per-frame RT budget.
            m_domBinRing[m_domRingHead] = bestPeakBin;
            m_domMagRing[m_domRingHead] = bestPeakBinMag;
            m_domRingHead = (m_domRingHead + 1) % SWEEP_RING_CAP;
            if (m_domRingCount < SWEEP_RING_CAP) ++m_domRingCount;
            ++m_framesSinceLastProvisionalEval;
            if (bestPeakBinMag > m_dominantFramePeakMag) {
                m_dominantFramePeakMag = bestPeakBinMag;
                // Persist the sub-band context of the snapshot so the
                // bandwidth walk anchors on the *winning band's* peak,
                // not a global arg-max that could belong to a louder
                // narrowband source elsewhere in-band.
                m_dominantAnchorBin    = bestPeakBin;
                m_dominantBandLoBin    = m_bands[bestBand].loBin;
                m_dominantBandHiBin    = m_bands[bestBand].hiBin;
                std::copy(magnitudes.begin(), magnitudes.end(),
                          m_dominantFrameMags.begin());
            }
        }
    } else {
        if (m_inEvent) {
            ++m_silenceFrames;
            if (m_silenceFrames > m_hangoverFrames) {
                const std::uint32_t endFrame =
                    currentFrame - static_cast<std::uint32_t>(m_silenceFrames);

                LS_DEBUG("dsp.bed", "event end frames=%u-%u %.1f-%.1fkHz peakSnr=%.2f%s",
                         m_eventStart, endFrame,
                         m_eventLoHz / 1000.0f, m_eventHiHz / 1000.0f,
                         m_eventPeakSnr,
                         m_gateRejected ? " [gate-rejected]" : "");

                // Finalise sweep-shape features over the whole event ring.
                // The provisional GATE_DECISION_FRAMES decision runs on
                // partial data — cricket-pulse onsets can look transiently
                // broadband before their narrow harmonic character emerges,
                // and early frames are partially-windowed / attenuated so a
                // provisional reject on ~8 ms of data is not trustworthy.
                // We re-evaluate the close-time gate UNCONDITIONALLY (see
                // A2) so the binding verdict comes from the full-event view
                // regardless of what the provisional gate decided.
                // Unroll the circular dominant-bin ring into temporal
                // order before evaluating. path_ratio and mono_fraction
                // are order-dependent (they consume per-frame steps);
                // reading the ring linearly after it wraps at 64 frames
                // (85 ms) introduces one artificial discontinuity at
                // the wrap point and computes the shape over a
                // temporally scrambled sequence. drift_khz and
                // bandwidth_khz are order-independent and unaffected.
                // Pattern mirrors the onset-ring unroll a few lines
                // below.
                std::size_t orderedDomBinsClose[SWEEP_RING_CAP];
                const std::size_t startClose =
                    (m_domRingCount < SWEEP_RING_CAP) ? 0 : m_domRingHead;
                for (std::size_t i = 0; i < m_domRingCount; ++i) {
                    orderedDomBinsClose[i] = m_domBinRing[
                        (startClose + i) % SWEEP_RING_CAP];
                }
                const SweepShape shape = computeSweepShape(
                    orderedDomBinsClose, m_domRingCount,
                    m_dominantFrameMags.data(), m_dominantFrameMags.size(),
                    m_dominantAnchorBin,
                    m_dominantBandLoBin, m_dominantBandHiBin,
                    m_binResolution);
                // --- Decision-path diagnostic state, hoisted so the
                //     sidecar drain (below) can record which rejection
                //     path fired. Since A2 the sweep-gate block runs
                //     unconditionally when the gate is enabled, so these
                //     are always populated on gate-on runs (previously
                //     `false`/`0` for provisionally-rejected events).
                bool     dxSweepBatLike = false;
                bool     dxVetoApplied  = false;
                float    dxRepRateHz    = 0.0f;
                float    dxRepCv        = 0.0f;
                uint16_t dxRepNOnsets   = 0;

                if (m_sweepGateEnabled) {
                    const bool sweepBatLike =
                        (shape.bandwidth_khz >= m_minBandwidthKhz)
                        || (shape.drift_khz     >= m_sweepDriftKhz
                            && shape.path_ratio    <= m_sweepPathRatioMax
                            && shape.mono_fraction >= m_sweepMonoFracMin);
                    dxSweepBatLike = sweepBatLike;

                    // --- Temporal repetition-rate guard ---
                    // The sweep-shape features alone can't reliably
                    // separate broadband crickets from real bats; the
                    // temporal signature usually can. Cricket trains
                    // cluster in the middle of the CV(IDI) axis, while
                    // metronomic bat feeding buzzes sit below and bursty
                    // passes sit above. This close-time veto only ever
                    // downgrades an accept to reject — it never
                    // resurrects a sweep-rejected event. The
                    // clearBat bypass (below) protects genuine
                    // wide-band FM bats (e.g. Myotis) or events whose
                    // own sweep shape is unambiguous, so the timing of
                    // surrounding events can't false-veto them.
                    bool metronomic = false;
                    RepStats rep{0.0f, 0.0f, 0};
                    if (m_repGuardEnabled && m_hopSizeSamples > 0
                        && m_sampleRate > 0) {
                        const float frameRateHz =
                            static_cast<float>(m_sampleRate)
                            / static_cast<float>(m_hopSizeSamples);
                        // Unroll the circular ring into temporal order.
                        // Before we wrap, the linear array happens to be
                        // in-order; after wrap, m_onsetFrames[m_onsetHead]
                        // is the OLDEST entry (about to be overwritten) and
                        // m_onsetFrames[(m_onsetHead-1)%CAP] is the newest.
                        // Reading it linearly after wrap trips
                        // computeRepStats' unsorted-onset safety guard and
                        // silently returns n=0 (no veto ever fires).
                        std::uint32_t ordered[ONSET_RING_CAP];
                        const std::size_t start =
                            (m_onsetCount < ONSET_RING_CAP) ? 0 : m_onsetHead;
                        for (std::size_t i = 0; i < m_onsetCount; ++i) {
                            ordered[i] = m_onsetFrames[
                                (start + i) % ONSET_RING_CAP];
                        }
                        rep = computeRepStats(ordered, m_onsetCount,
                                              frameRateHz);
                        metronomic = (rep.n >= static_cast<std::size_t>(m_repMinEvents))
                                     && (rep.rate_hz >= m_repRateMinHz)
                                     && (rep.rate_hz <= m_repRateMaxHz)
                                     && (rep.cv_idi   >= m_repCvMin)
                                     && (rep.cv_idi   <= m_repCvMax);
                        dxRepRateHz  = rep.rate_hz;
                        dxRepCv      = rep.cv_idi;
                        dxRepNOnsets = static_cast<std::uint16_t>(
                            std::min<std::size_t>(rep.n,
                                std::numeric_limits<std::uint16_t>::max()));
                    }
                    // Protect any event whose *own* sweep shape marks it as a
                    // clear bat call — either a wide-band event (bw >= keep
                    // threshold) or a smooth FM sweep (drift + path + mono
                    // clause). Without the sweep-shape bypass, mixed
                    // cricket+bat clips lose real bat events when the
                    // onset ring is cricket-dominated: the ring looks
                    // metronomic, so a bat event that happens to close
                    // during it gets vetoed even though its own signature
                    // is clearly a sweep.
                    const bool clearBat =
                        (shape.bandwidth_khz >= m_repBroadbandKeepKhz)
                        || (shape.drift_khz     >= m_sweepDriftKhz
                            && shape.path_ratio    <= m_sweepPathRatioMax
                            && shape.mono_fraction >= m_sweepMonoFracMin);
                    const bool batLike = sweepBatLike
                                         && !(metronomic && !clearBat);
                    // "veto_applied" = the sweep-shape said keep but the
                    // temporal guard flipped it. This is the exact
                    // population the followup2 analysis needs to bucket.
                    dxVetoApplied = sweepBatLike && !batLike;

                    if (!batLike) {
                        m_gateRejected = true;
                        const char* why = sweepBatLike ? "temporal" : "sweep";
                        LS_DEBUG("dsp.bed",
                                 "event rejected by %s gate (close) "
                                 "bw=%.2fkHz drift=%.1fkHz path=%.2f mono=%.2f "
                                 "rate=%.2fHz cv=%.2f onsets=%zu",
                                 why,
                                 shape.bandwidth_khz, shape.drift_khz,
                                 shape.path_ratio, shape.mono_fraction,
                                 rep.rate_hz, rep.cv_idi, rep.n);
                    }
                }

                // Finalise and stash this event's diagnostics for the next
                // drainSidecarPayload() call. Rejected events are still
                // recorded here (with gate_rejected=true) so the sidecar
                // reveals what the gate suppressed — the validator can
                // then A/B compare without a rebuild.
                {
                    std::lock_guard<std::mutex> lk(m_diagnosticsMutex);
                    m_inProgressEvent.end_frame       = endFrame;
                    m_inProgressEvent.duration_frames = static_cast<std::uint16_t>(
                        std::min<std::uint32_t>(
                            endFrame - m_eventStart + 1u,
                            std::numeric_limits<std::uint16_t>::max()));
                    m_inProgressEvent.lo_hz         = m_eventLoHz;
                    m_inProgressEvent.hi_hz         = m_eventHiHz;
                    m_inProgressEvent.peak_snr      = m_eventPeakSnr;
                    m_inProgressEvent.bandwidth_khz = shape.bandwidth_khz;
                    m_inProgressEvent.drift_khz     = shape.drift_khz;
                    m_inProgressEvent.path_ratio    = shape.path_ratio;
                    m_inProgressEvent.mono_fraction = shape.mono_fraction;
                    m_inProgressEvent.gate_rejected = m_gateRejected;
                    // Decision-path diagnostics (observability, no
                    // behaviour change). If the sweep-gate block was
                    // skipped because a provisional decision already
                    // rejected this event, `m_provisionalRejectedThisEvent`
                    // stays true and the sidecar reflects that. Otherwise
                    // the local dx* values were populated above.
                    m_inProgressEvent.sweep_bat_like       = dxSweepBatLike;
                    m_inProgressEvent.veto_applied         = dxVetoApplied;
                    m_inProgressEvent.provisional_rejected =
                        m_provisionalRejectedThisEvent;
                    m_inProgressEvent.rep_rate_hz          = dxRepRateHz;
                    m_inProgressEvent.rep_cv               = dxRepCv;
                    m_inProgressEvent.rep_n_onsets         = dxRepNOnsets;
                    m_pendingEvents.push_back(m_inProgressEvent);
                }

                // --- Publish the per-event verdict to the recorder ---
                // Exactly one counter increments per closed event. The
                // recorder observes m_batLikeEventsSinceBoot moving between
                // beginRecording() and endRecording() to distinguish a
                // pure-cricket clip (no bat-like event) from a real bat pass.
                // Kept outside the diagnostics mutex so the recorder never
                // waits on the audio thread.
                if (m_gateRejected) {
                    m_rejectedEventsSinceBoot.fetch_add(1, std::memory_order_relaxed);
                } else {
                    m_batLikeEventsSinceBoot.fetch_add(1, std::memory_order_relaxed);
                }

                // The Annotation return path is downstream of the recorder's
                // outState.active gate, so we mirror the suppression here
                // too: if the gate rejected this event, consume it silently
                // instead of emitting a closed-event annotation. Otherwise
                // the offline validator (which counts annotations) would
                // see rejected events and gate-off vs gate-on would look
                // identical, defeating the point of the gate.
                if (!m_gateRejected) {
                    outAnnotation.start_frame = m_eventStart;
                    outAnnotation.end_frame   = endFrame;
                    outAnnotation.low_freq    = m_eventLoHz;
                    outAnnotation.high_freq   = m_eventHiHz;
                    emittedAnnotation = true;
                }

                m_inEvent          = false;
                m_activeRun        = 0;
                m_silenceFrames    = 0;
                m_eventPeakSnr     = 0.0f;
                m_provisionalSuppressed          = false;
                m_gateRejected                   = false;
                m_provisionalRejectedThisEvent   = false;
                m_framesSinceLastProvisionalEval = 0;
            }
        } else {
            m_activeRun = 0;
        }
    }

    // 5. Sweep-shape gate — periodic provisional suppression.
    //
    // Fires whenever GATE_DECISION_FRAMES fresh ring frames have arrived
    // since the last provisional evaluation. The first fire lands
    // naturally at ~8 ms (6 hot frames from event open) and re-runs at
    // the same cadence for the rest of the event — so a bat call
    // arriving inside a still-open cricket event has a chance to flip
    // @c m_provisionalSuppressed back OFF and reopen the recorder's
    // window. This is a *non-binding* flag: it drives outState.active
    // only. The binding gate_rejected verdict is set at close-time from
    // the full-event ring, independently.
    if (m_sweepGateEnabled && m_inEvent
        && m_domRingCount >= GATE_DECISION_FRAMES
        && m_framesSinceLastProvisionalEval >= GATE_DECISION_FRAMES) {
        // Unroll the circular dominant-bin ring into temporal order —
        // see A4 comment at the close-time call site above.
        std::size_t orderedDomBins[SWEEP_RING_CAP];
        const std::size_t start =
            (m_domRingCount < SWEEP_RING_CAP) ? 0 : m_domRingHead;
        for (std::size_t i = 0; i < m_domRingCount; ++i) {
            orderedDomBins[i] = m_domBinRing[
                (start + i) % SWEEP_RING_CAP];
        }
        const SweepShape s = computeSweepShape(
            orderedDomBins, m_domRingCount,
            m_dominantFrameMags.data(), m_dominantFrameMags.size(),
            m_dominantAnchorBin,
            m_dominantBandLoBin, m_dominantBandHiBin,
            m_binResolution);
        const bool batLike =
            (s.bandwidth_khz >= m_minBandwidthKhz)
            || (s.drift_khz     >= m_sweepDriftKhz
                && s.path_ratio    <= m_sweepPathRatioMax
                && s.mono_fraction >= m_sweepMonoFracMin);
        const bool wasSuppressed = m_provisionalSuppressed;
        m_provisionalSuppressed = !batLike;
        m_framesSinceLastProvisionalEval = 0;
        // Diagnostic: mirror the CURRENT provisional state so the sidecar
        // reflects the event's final suppression, not "was ever suppressed
        // at least once". A3 made suppression re-evaluable, so an event
        // that was suppressed early and then re-opened would otherwise
        // still be labelled provisional_rejected in the sidecar — which
        // put ~10 k re-opened clips in followup2's provisional_only
        // bucket even though tuning the provisional gate would recover
        // nothing (the events are already being reopened). Observability
        // only; no filter behaviour change.
        m_provisionalRejectedThisEvent = m_provisionalSuppressed;
        if (m_provisionalSuppressed && !wasSuppressed) {
            LS_DEBUG("dsp.bed",
                     "event provisionally suppressed bw=%.2fkHz drift=%.1fkHz "
                     "path=%.2f mono=%.2f",
                     s.bandwidth_khz, s.drift_khz, s.path_ratio, s.mono_fraction);
        } else if (!m_provisionalSuppressed && wasSuppressed) {
            LS_DEBUG("dsp.bed",
                     "event provisionally re-opened bw=%.2fkHz drift=%.1fkHz "
                     "path=%.2f mono=%.2f",
                     s.bandwidth_khz, s.drift_khz, s.path_ratio, s.mono_fraction);
        }
    }

    // 6. Per-frame state snapshot for the downstream recorder.
    // Uses @c m_provisionalSuppressed (the non-binding fast-drop flag).
    // The binding @c m_gateRejected verdict is set at event close and
    // propagates via the batLike/rejected counters the recorder polls
    // at endRecording — it does not gate per-frame reporting here.
    const bool reportActive = m_inEvent && !m_provisionalSuppressed;
    outState.active = reportActive;
    outState.lo_hz  = reportActive ? m_eventLoHz : 0.0f;
    outState.hi_hz  = reportActive ? m_eventHiHz : 0.0f;

    return emittedAnnotation;
}

// --- Pure helper: sweep-shape feature extraction --------------------------
//
// All inputs come from the per-event ring + dominant-frame snapshot the hot
// loop populates. This function has no per-call allocation and no mutable
// state of its own, so it's directly unit-testable from tests/unit without
// standing up a detector instance.
BandEnergyDetector::SweepShape BandEnergyDetector::computeSweepShape(
        const std::size_t* domBins,
        std::size_t        count,
        const float*       dominantFrameMags,
        std::size_t        numBins,
        std::size_t        anchorBin,
        std::size_t        bandLoBin,
        std::size_t        bandHiBin,
        float              binResolutionHz) {
    SweepShape out{0.0f, 0.0f, 0.0f, 0.0f};
    if (count == 0) return out;

    // --- drift: span of the per-frame dominant bin, in kHz ---
    std::size_t domMin = domBins[0];
    std::size_t domMax = domBins[0];
    for (std::size_t i = 1; i < count; ++i) {
        if (domBins[i] < domMin) domMin = domBins[i];
        if (domBins[i] > domMax) domMax = domBins[i];
    }
    const std::size_t range = domMax - domMin;
    out.drift_khz = (static_cast<float>(range) * binResolutionHz) / 1000.0f;

    // --- path_ratio + mono_fraction over per-frame steps ---
    std::size_t pathSum  = 0;
    std::size_t upSteps  = 0;
    std::size_t downSteps = 0;
    for (std::size_t i = 1; i < count; ++i) {
        if (domBins[i] > domBins[i - 1]) {
            pathSum += (domBins[i] - domBins[i - 1]);
            ++upSteps;
        } else if (domBins[i] < domBins[i - 1]) {
            pathSum += (domBins[i - 1] - domBins[i]);
            ++downSteps;
        }
    }
    const std::size_t denom = (range == 0) ? 1u : range;
    out.path_ratio = static_cast<float>(pathSum) / static_cast<float>(denom);

    const std::size_t totalSteps = upSteps + downSteps;
    if (totalSteps > 0) {
        const std::size_t maxDir = (upSteps >= downSteps) ? upSteps : downSteps;
        out.mono_fraction = static_cast<float>(maxDir)
                            / static_cast<float>(totalSteps);
    } else {
        // No transitions => trivially monotone (single-bin event).
        out.mono_fraction = 1.0f;
    }

    // --- 10-dB bandwidth at the dominant frame ---
    // 10 dB in amplitude = factor of sqrt(10). Walk left/right from
    // @c anchorBin (the winning sub-band's dominant bin at the loudest
    // snapshot frame) until magnitude drops below refMag/sqrt(10), then
    // report the width in kHz. Confined to @c [bandLoBin, bandHiBin) so
    // the walk never leaves the winning sub-band — a re-scanned arg-max
    // over the full in-band range would jump to a louder narrowband
    // source (e.g. a nearby cricket) and measure *its* bandwidth,
    // producing a spuriously narrow result for the actual bat event.
    if (numBins > 0 && bandHiBin > bandLoBin && bandHiBin <= numBins
        && anchorBin >= bandLoBin && anchorBin < bandHiBin) {
        const float refMag = dominantFrameMags[anchorBin];
        if (refMag > 0.0f) {
            const float threshold = refMag / std::sqrt(10.0f);
            std::size_t leftBin = anchorBin;
            while (leftBin > bandLoBin
                   && dominantFrameMags[leftBin - 1] > threshold) {
                --leftBin;
            }
            std::size_t rightBin = anchorBin;
            while (rightBin + 1 < bandHiBin
                   && dominantFrameMags[rightBin + 1] > threshold) {
                ++rightBin;
            }
            out.bandwidth_khz = (static_cast<float>(rightBin - leftBin)
                                 * binResolutionHz) / 1000.0f;
        }
    }

    return out;
}

// --- Pure helper: temporal repetition-rate statistic ---------------------
//
// Cricket pulse trains have a metronomic inter-onset interval (IDI); real
// bat passes are bursty. This helper computes the two axes the temporal
// guard reads: pulse rate in Hz and CV(IDI). Pure, unit-testable without
// a detector instance; the hot loop provides the onset frame indices from
// its cross-event ring.
//
// Numerics: intervals are computed in double so long onset spans don't
// lose precision to float rounding; the returned floats are safely within
// the metronomic-guard tunable resolution.
BandEnergyDetector::RepStats BandEnergyDetector::computeRepStats(
        const std::uint32_t* onsetFrames,
        std::size_t          count,
        float                frameRateHz) {
    RepStats out{0.0f, 0.0f, 0};
    // Distinguish "no ring pointer at all" (a caller bug — return n=0 so
    // the guard treats it as insufficient history and never trusts the
    // stat) from "ring is valid but too short" (n = count, so the caller
    // can compare against rep_min_events).
    if (!onsetFrames) return out;
    if (count < 2 || frameRateHz <= 0.0f) {
        out.n = count;
        return out;
    }

    const double dt_per_frame = 1.0 / static_cast<double>(frameRateHz);
    double sum  = 0.0;
    double sum2 = 0.0;
    const std::size_t n_ivals = count - 1;
    for (std::size_t i = 1; i < count; ++i) {
        // std::uint32_t subtraction wraps on backward jumps; guard against a
        // caller passing an unsorted or wrapped ring. In that case the whole
        // stat is untrustworthy — return zeros with n=count so the guard
        // treats "insufficient history".
        if (onsetFrames[i] < onsetFrames[i - 1]) {
            out.n = 0;
            return out;
        }
        const double d = static_cast<double>(onsetFrames[i] - onsetFrames[i - 1])
                         * dt_per_frame;
        sum  += d;
        sum2 += d * d;
    }
    const double mean = sum / static_cast<double>(n_ivals);
    if (sum <= 0.0 || mean <= 0.0) {
        out.n = count;
        return out;
    }
    const double var = std::max(0.0, (sum2 / static_cast<double>(n_ivals))
                                     - mean * mean);
    const double sd  = std::sqrt(var);

    out.n       = count;
    out.rate_hz = static_cast<float>(1.0 / mean);
    out.cv_idi  = static_cast<float>(sd / mean);
    return out;
}

// Keep setTunable / getTunable / listTunables in lock-step. Every new tunable
// needs an entry in all three — with ~10 knobs and a setTunable() that rejects
// unknown keys the drift surface is small, but listTunables() in particular
// is the source the GUI form is rendered from, so an omission silently hides
// a knob rather than failing loudly.

bool BandEnergyDetector::setTunable(const char* key, double value) {
    if (!key) return false;
    if (std::strcmp(key, "alpha_rise")           == 0) { m_alphaRise         = static_cast<float>(value); return true; }
    if (std::strcmp(key, "alpha_fall")           == 0) { m_alphaFall         = static_cast<float>(value); return true; }
    if (std::strcmp(key, "min_abs_floor")        == 0) { m_minAbsFloor       = static_cast<float>(value); return true; }
    if (std::strcmp(key, "band_snr_threshold")   == 0) { m_bandSnrThreshold  = static_cast<float>(value); return true; }
    if (std::strcmp(key, "min_flatness")         == 0) { m_minFlatness       = static_cast<float>(value); return true; }
    if (std::strcmp(key, "max_flatness")         == 0) { m_maxFlatness       = static_cast<float>(value); return true; }
    if (std::strcmp(key, "top_k")                == 0) { m_topK              = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "warmup_frames")        == 0) { m_warmupFramesLimit = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "min_active_frames")    == 0) { m_minActiveFrames   = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "hangover_frames")      == 0) { m_hangoverFrames    = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "sweep_gate_enabled")   == 0) { m_sweepGateEnabled  = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "min_bandwidth_khz")    == 0) { m_minBandwidthKhz   = static_cast<float>(value); return true; }
    if (std::strcmp(key, "sweep_drift_khz")      == 0) { m_sweepDriftKhz     = static_cast<float>(value); return true; }
    if (std::strcmp(key, "sweep_path_ratio_max") == 0) { m_sweepPathRatioMax = static_cast<float>(value); return true; }
    if (std::strcmp(key, "sweep_mono_frac_min")  == 0) { m_sweepMonoFracMin  = static_cast<float>(value); return true; }
    // Temporal repetition-rate guard
    if (std::strcmp(key, "rep_guard_enabled")     == 0) { m_repGuardEnabled     = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "rep_rate_min_hz")       == 0) { m_repRateMinHz        = static_cast<float>(value); return true; }
    if (std::strcmp(key, "rep_rate_max_hz")       == 0) { m_repRateMaxHz        = static_cast<float>(value); return true; }
    if (std::strcmp(key, "rep_cv_min")            == 0) { m_repCvMin            = static_cast<float>(value); return true; }
    if (std::strcmp(key, "rep_cv_max")            == 0) { m_repCvMax            = static_cast<float>(value); return true; }
    if (std::strcmp(key, "rep_min_events")        == 0) { m_repMinEvents        = static_cast<int>(value);   return true; }
    if (std::strcmp(key, "rep_broadband_keep_khz")== 0) { m_repBroadbandKeepKhz = static_cast<float>(value); return true; }
    if (std::strcmp(key, "hop_size_samples")      == 0) { m_hopSizeSamples      = static_cast<int>(value);   return true; }
    return false;
}

bool BandEnergyDetector::getTunable(const char* key, double* outValue) const {
    if (!key || !outValue) return false;
    if (std::strcmp(key, "alpha_rise")           == 0) { *outValue = m_alphaRise;         return true; }
    if (std::strcmp(key, "alpha_fall")           == 0) { *outValue = m_alphaFall;         return true; }
    if (std::strcmp(key, "min_abs_floor")        == 0) { *outValue = m_minAbsFloor;       return true; }
    if (std::strcmp(key, "band_snr_threshold")   == 0) { *outValue = m_bandSnrThreshold;  return true; }
    if (std::strcmp(key, "min_flatness")         == 0) { *outValue = m_minFlatness;       return true; }
    if (std::strcmp(key, "max_flatness")         == 0) { *outValue = m_maxFlatness;       return true; }
    if (std::strcmp(key, "top_k")                == 0) { *outValue = m_topK;              return true; }
    if (std::strcmp(key, "warmup_frames")        == 0) { *outValue = m_warmupFramesLimit; return true; }
    if (std::strcmp(key, "min_active_frames")    == 0) { *outValue = m_minActiveFrames;   return true; }
    if (std::strcmp(key, "hangover_frames")      == 0) { *outValue = m_hangoverFrames;    return true; }
    if (std::strcmp(key, "sweep_gate_enabled")   == 0) { *outValue = m_sweepGateEnabled;  return true; }
    if (std::strcmp(key, "min_bandwidth_khz")    == 0) { *outValue = m_minBandwidthKhz;   return true; }
    if (std::strcmp(key, "sweep_drift_khz")      == 0) { *outValue = m_sweepDriftKhz;     return true; }
    if (std::strcmp(key, "sweep_path_ratio_max") == 0) { *outValue = m_sweepPathRatioMax; return true; }
    if (std::strcmp(key, "sweep_mono_frac_min")  == 0) { *outValue = m_sweepMonoFracMin;  return true; }
    if (std::strcmp(key, "rep_guard_enabled")     == 0) { *outValue = m_repGuardEnabled;     return true; }
    if (std::strcmp(key, "rep_rate_min_hz")       == 0) { *outValue = m_repRateMinHz;        return true; }
    if (std::strcmp(key, "rep_rate_max_hz")       == 0) { *outValue = m_repRateMaxHz;        return true; }
    if (std::strcmp(key, "rep_cv_min")            == 0) { *outValue = m_repCvMin;            return true; }
    if (std::strcmp(key, "rep_cv_max")            == 0) { *outValue = m_repCvMax;            return true; }
    if (std::strcmp(key, "rep_min_events")        == 0) { *outValue = m_repMinEvents;        return true; }
    if (std::strcmp(key, "rep_broadband_keep_khz")== 0) { *outValue = m_repBroadbandKeepKhz; return true; }
    if (std::strcmp(key, "hop_size_samples")      == 0) { *outValue = m_hopSizeSamples;      return true; }
    return false;
}

std::span<const TunableInfo> BandEnergyDetector::listTunables() const {
    // Static so the returned pointers stay valid for the plugin's lifetime —
    // the validator's C API and Python wrapper pass these strings across the
    // FFI boundary unchanged. Lives in .rodata; zero per-call cost.
    static constexpr TunableInfo kTunables[] = {
        {"band_snr_threshold",   TunableType::Float, 8.0,   0.0,    200.0,
         "Top-K mean band SNR above which a frame counts as hot."},
        {"min_flatness",         TunableType::Float, 0.10,  0.0,    1.0,
         "Spectral-flatness lower bound (rejects pure tones)."},
        {"max_flatness",         TunableType::Float, 0.80,  0.0,    1.0,
         "Spectral-flatness upper bound (rejects broadband noise)."},
        {"top_k",                TunableType::Int,   8.0,   1.0,    64.0,
         "Number of brightest bins per band averaged for the SNR statistic."},
        {"min_active_frames",    TunableType::Int,   2.0,   1.0,    100.0,
         "Consecutive hot frames required before an event opens (debounce)."},
        {"hangover_frames",      TunableType::Int,   8.0,   1.0,    200.0,
         "Consecutive quiet frames tolerated inside an event before it closes."},
        {"warmup_frames",        TunableType::Int,   40.0,  0.0,    1000.0,
         "Frames discarded after start while the noise-floor EMA seeds."},
        {"alpha_rise",           TunableType::Float, 0.995, 0.0,    1.0,
         "EMA coefficient when the floor is rising (slow)."},
        {"alpha_fall",           TunableType::Float, 0.90,  0.0,    1.0,
         "EMA coefficient when the floor is falling (fast)."},
        {"min_abs_floor",        TunableType::Float, 1e-6,  0.0,    1.0,
         "Absolute lower bound on the noise floor (prevents divide-by-zero spikes)."},
        {"sweep_gate_enabled",   TunableType::Int,   1.0,   0.0,    1.0,
         "Master switch (0/1) for the cricket sweep-shape gate."},
        {"min_bandwidth_khz",    TunableType::Float, 0.9,   0.0,    50.0,
         "Keep events whose 10-dB bandwidth at the dominant frame is at least this wide."},
        {"sweep_drift_khz",      TunableType::Float, 8.0,   0.0,    200.0,
         "Min dominant-frequency excursion (kHz) for an event to qualify as a smooth sweep."},
        {"sweep_path_ratio_max", TunableType::Float, 1.6,   1.0,    10.0,
         "Max total-travel/net-range ratio for a sweep to count as smooth (>~2 = hopping)."},
        {"sweep_mono_frac_min",  TunableType::Float, 0.7,   0.0,    1.0,
         "Min fraction of dominant-bin steps in a single direction for a smooth sweep."},
        {"rep_guard_enabled",    TunableType::Int,   0.0,   0.0,    1.0,
         "Master switch (0/1) for the temporal repetition-rate guard. "
         "Default 0 (off) as of the veto-recovery pre-release; set to 1 "
         "for CF-heavy sites where metronomic-call rejection matters."},
        {"rep_rate_min_hz",      TunableType::Float, 1.0,   0.0,    100.0,
         "Lower bound of the cricket pulse-rate band (Hz)."},
        {"rep_rate_max_hz",      TunableType::Float, 20.0,  0.0,    200.0,
         "Upper bound of the cricket pulse-rate band (Hz)."},
        {"rep_cv_min",           TunableType::Float, 0.50,  0.0,    2.0,
         "Min CV(inter-onset interval) for the cricket band. "
         "Below this looks like a metronomic bat feeding buzz, not a cricket."},
        {"rep_cv_max",           TunableType::Float, 1.30,  0.0,    2.0,
         "Max CV(inter-onset interval) for the cricket band. "
         "Above this looks like a bursty bat pass (e.g. Barbastella)."},
        {"rep_min_events",       TunableType::Int,   2.0,   2.0,    16.0,
         "Onsets required before the temporal verdict is trusted."},
        {"rep_broadband_keep_khz", TunableType::Float, 10.0, 0.0,   50.0,
         "Bandwidth above which an event is never vetoed on temporal grounds."},
        {"hop_size_samples",     TunableType::Int,   512.0, 1.0,    65535.0,
         "FFT hop in samples (set by the pipeline). Used to convert onset "
         "frame indices into seconds for the temporal-guard rate bounds."},
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

// The ladder is anchored on `balanced`, which must always reproduce the
// compiled defaults exactly — it is documented as "Echobox defaults" and
// an operator who applies it expects a no-op. When the shipped defaults
// moved to snr 8.0 / flat 0.80 they took `balanced` with them, which in
// turn pushed `quiet` down a step so the ladder keeps three distinct
// rungs rather than collapsing into "default, default-ish, strict".
constexpr PresetEntry kQuietBundle[] = {
    // One step more sensitive than the default on every axis. snr 6.0 is
    // where the recall curve saturates (~98 % / ~98 % per-call, vs 97.4 /
    // 97.9 at snr 8) while duty keeps climbing — worth it only at a site
    // quiet enough to afford the bytes. min_active_frames 1 adds a
    // measured +1.0 pp recall for +0.9 pp duty and raises FP risk, which
    // is the trade this preset exists to make.
    {"band_snr_threshold",  6.0},
    {"max_flatness",        0.80},
    {"min_active_frames",   1.0},
};
constexpr PresetEntry kBalancedBundle[] = {
    // Must mirror the compiled defaults in BandEnergyDetector.hpp.
    {"band_snr_threshold",  8.0},
    {"max_flatness",        0.80},
    {"min_active_frames",   2.0},
};
constexpr PresetEntry kNoisyBundle[] = {
    // Deliberately far stricter than the default: at snr ~20 per-call
    // recall drops to 80.8 / 85.1 % but duty falls to ~11 %. A site with
    // wind or road noise spends its byte budget on false positives
    // otherwise. max_flatness 0.60 stays below the 0.80 default because
    // broadband rejection is exactly what a noisy site needs most.
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
