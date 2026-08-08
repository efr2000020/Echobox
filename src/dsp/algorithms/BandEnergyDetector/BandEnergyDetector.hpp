// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Wide-band bat-presence detector implementation. Ships built-in on
/// production builds; loadable as a plugin on dev builds.

#include "../../ISweepTracker.hpp"  // src/dsp/algorithms/BandEnergyDetector/ -> src/dsp/
#include <atomic>
#include <mutex>
#include <span>
#include <cstdint>
#include <vector>

/**
 * @brief Wide-band bat *presence* detector.
 *
 * Integrates energy across several sub-bands spanning the configured
 * detection window. A frame is "active" if any sub-band's energy rises
 * sufficiently above that sub-band's own adaptive noise floor.
 *
 * Why this design:
 *  - Steep-FM bats (e.g. Myotis) sweep across 100+ kHz within a single FFT
 *    frame. A single-peak search picks one bin and discards the rest of the
 *    sweep, and tends to lock onto the loud low-frequency tail — so faint
 *    high-frequency calls get masked. Energy integration has no "winner": a
 *    high-frequency sweep contributes its energy and is judged on its merit.
 *  - Per-sub-band floors mean each frequency region gets a fair, level-
 *    appropriate threshold. High-frequency calls are physically fainter
 *    (atmospheric absorption rises with frequency) and sit on a much lower
 *    noise floor; a per-band SNR ratio handles both automatically, where a
 *    single absolute threshold could not.
 *
 * Emits the standard @c Annotation (start/end frame + freq span of the
 * firing sub-band) when an event closes, and publishes a per-frame
 * @c DetectorState so the downstream recorder can react on the leading edge.
 */
class BandEnergyDetector : public ISweepTracker {
public:
    BandEnergyDetector();
    ~BandEnergyDetector() override = default;

    void configure(int sampleRate, std::size_t fftSize,
                   float freqLoHz, float freqHiHz) override;

    bool processFrame(std::span<const float> magnitudes,
                      std::uint32_t currentFrame,
                      DetectorState& outState,
                      Annotation& outAnnotation) override;

    bool setTunable(const char* key, double value) override;
    bool getTunable(const char* key, double* outValue) const override;
    std::span<const TunableInfo> listTunables() const override;
    bool applyPreset(const char* name) override;
    std::span<const PresetInfo>  listPresets()  const override;

    bool          drainSidecarPayload(SidecarPayload& out) override;
    bool          seedNoiseFloor(std::span<const float> floor) override;
    std::uint64_t totalEventsSinceBoot() const override;

    /**
     * @brief Wait-free counter of events kept by the sweep gate since boot.
     *
     * Read by the DSP pipeline every publish() cycle and propagated to the
     * recorder through @c DetectorStateSnapshot::batLikeEvents. The recorder
     * snapshots this value on @c beginRecording and, at @c endRecording,
     * discards the clip if it hasn't advanced (no bat-like event fired
     * during the clip window).
     *
     * Wait-free by design (single atomic load): safe to call from any thread,
     * including the recorder's poll loop.
     */
    std::uint64_t batLikeEventsSinceBoot() const override {
        return m_batLikeEventsSinceBoot.load(std::memory_order_relaxed);
    }
    /// Counter of events rejected by the sweep gate. Debugging convenience;
    /// wait-free like @c batLikeEventsSinceBoot().
    std::uint64_t rejectedEventsSinceBoot() const {
        return m_rejectedEventsSinceBoot.load(std::memory_order_relaxed);
    }

    // --- Sweep-shape gate helper ---
    // Exposed publicly so the unit tests can drive it directly without
    // standing up the whole detector. Inputs come from the per-event ring
    // populated inside processFrame(); it is a pure function of its args.
    struct SweepShape {
        float bandwidth_khz;     // 10-dB bandwidth at the dominant frame
        float drift_khz;         // (max-min) dominant bin span, in kHz
        float path_ratio;        // sum(|Δbin|) / max(1, range)
        float mono_fraction;     // max(#up,#down) / (#up+#down)
    };
    // The 10-dB bandwidth walk is anchored on @c anchorBin (the event's
    // per-frame dominant bin at the loudest snapshot) and confined to
    // @c [bandLoBin, bandHiBin) — the winning sub-band's range at that
    // snapshot. Both are required: band selection upstream is by top-K
    // SNR, so a distant bat can win the sub-band while a nearby cricket
    // owns the global magnitude peak; anchoring on the sub-band peak
    // ensures the walk measures the winning event's own bandwidth. The
    // walk's reference magnitude is @c dominantFrameMags[anchorBin], so
    // the origin and the drop-threshold come from the same signal.
    static SweepShape computeSweepShape(const std::size_t* domBins,
                                        std::size_t        count,
                                        const float*       dominantFrameMags,
                                        std::size_t        numBins,
                                        std::size_t        anchorBin,
                                        std::size_t        bandLoBin,
                                        std::size_t        bandHiBin,
                                        float              binResolutionHz);

    // --- Repetition-rate helper (temporal cricket rejection) ---
    // The sweep-shape features can't reliably separate broadband crickets
    // from real bats — but the temporal signature usually can. Cricket
    // trains cluster in the middle of the CV(inter-onset interval) axis
    // (roughly 0.5-1.3 with rates 1-20 Hz on this detector); metronomic
    // bat feeding buzzes sit below that band and bursty passes above it.
    // This helper is pure, unit-testable without a detector instance;
    // the hot loop just feeds it the onset frames it has already recorded.
    struct RepStats {
        float       rate_hz;    // (n-1) / total elapsed
        float       cv_idi;     // stddev(inter-onset interval) / mean(idi)
        std::size_t n;          // onset count used
    };
    static RepStats computeRepStats(const std::uint32_t* onsetFrames,
                                    std::size_t          count,
                                    float                frameRateHz);

private:
    int         m_sampleRate;
    std::size_t m_fftSize;
    float       m_binResolution;
    float       m_freqLoHz;
    float       m_freqHiHz;

    // --- Sub-band layout ---
    // Internal partition of the detection range. Edges are clamped to the
    // configured [freqLoHz, freqHiHz] window at configure() time; bands that
    // fall entirely outside the window are dropped.
    static constexpr int   MAX_BANDS = 4;
    static constexpr float BAND_EDGES_HZ[MAX_BANDS + 1] = {
        20000.0f, 45000.0f, 80000.0f, 130000.0f, 192000.0f
    };

    struct Band {
        std::size_t loBin;
        std::size_t hiBin;
    };
    std::vector<Band> m_bands;

    // In-band range over which spectral flatness is computed.
    std::size_t m_inBandLo;
    std::size_t m_inBandHi;

    // --- Per-bin adaptive noise floor (EMA) ---
    // Asymmetric: rises slowly (a call barely lifts its own floor), falls fast
    // (tracks genuine quieting). One estimate per FFT bin, shared across bands.
    std::vector<float> m_noiseFloor;
    bool               m_floorSeeded;
    int                m_warmupFrames;

    // Scratch buffer for the per-band top-K SNR statistic (pre-allocated; no
    // per-frame allocation, so processFrame stays real-time safe).
    std::vector<float> m_bandSnrScratch;

    // --- Temporal state machine (debounce on the way in, hangover on the way out) ---
    bool          m_inEvent;
    int           m_activeRun;       // consecutive hot frames before an event opens
    int           m_silenceFrames;   // consecutive quiet frames since last hot frame
    std::uint32_t m_eventStart;
    float         m_eventLoHz;
    float         m_eventHiHz;
    // Peak top-K mean band SNR observed during the currently open event.
    // Logged on event-end so each saved file can be correlated with how
    // emphatically the detector thought it was a call.
    float         m_eventPeakSnr;

    // Diagnostics
    int   m_frameCount;
    float m_maxBandSnrInHeartbeat;
    int   m_peakBandInHeartbeat;

    // --- Tuning parameters (validated against real recordings; see project HANDOFF) ---
    // Defaults are the ship values; the offline validator reaches in via
    // setTunable() to grid-search without recompiling.
    float m_alphaRise         = 0.995f;
    float m_alphaFall         = 0.90f;
    float m_minAbsFloor       = 1e-6f;

    int   m_topK              = 8;
    float m_bandSnrThreshold  = 12.0f;

    float m_minFlatness       = 0.10f;
    float m_maxFlatness       = 0.65f;

    int   m_warmupFramesLimit = 40;
    int   m_minActiveFrames   = 2;
    int   m_hangoverFrames    = 8;

    // --- Sweep-shape gate (cricket false-positive rejection) ---
    // Master switch: 1 = gate active. When 0, both outState.active and the
    // closed-event Annotation are byte-identical to what the detector would
    // emit with no gate at all, so an operator can turn the gate off with
    // a single tunable if a deployment site produces bat calls the gate
    // can't characterise.
    //
    // Bandwidth threshold at 0.9 kHz preserves 25/25 clean-corpus recall
    // (including NYCLEI 3/3 and NYCNOC 2/2). The in-band bandwidth walk
    // is measured on the winning-band's dominant frame; CF/QCF species
    // (e.g. Nyctalus noctula) sit close to this floor, so raising it
    // costs recall.
    int   m_sweepGateEnabled  = 1;
    float m_minBandwidthKhz   = 0.9f;   // bat-like if 10-dB BW >= this
    float m_sweepDriftKhz     = 8.0f;   // OR a smooth sweep of this much drift
    float m_sweepPathRatioMax = 1.6f;   //    that doesn't hop (low travel/range)
    float m_sweepMonoFracMin  = 0.7f;   //    and runs mostly in one direction

    // Per-event ring of dominant bins + magnitude-frame snapshot, used to
    // compute sweep-shape features at gate-decision time and at event close.
    // Fixed capacity (≈ 85 ms at 750 fps); pre-allocated in configure() and
    // never grown inside processFrame(). Events longer than the cap keep the
    // most recent N frames — a long event is bat-like under existing heuristics
    // and is not the gate's primary concern.
    static constexpr std::size_t SWEEP_RING_CAP        = 64;
    static constexpr std::size_t GATE_DECISION_FRAMES  = 6; // ~8 ms at 750 fps
    std::size_t        m_domBinRing[SWEEP_RING_CAP];
    float              m_domMagRing[SWEEP_RING_CAP];
    std::size_t        m_domRingCount;       // valid entries (caps at SWEEP_RING_CAP)
    std::size_t        m_domRingHead;        // next write index (circular)
    std::vector<float> m_dominantFrameMags;  // spectrum snapshot at loudest dom frame
    float              m_dominantFramePeakMag;
    // Anchor for the 10-dB bandwidth walk. Captured at the same instant
    // as @c m_dominantFrameMags (the "loudest snapshot frame"). Anchoring
    // on the *winning sub-band's* dominant bin — not a re-scan across the
    // full 20-192 kHz range — keeps the walk on the event's own peak;
    // otherwise a louder narrowband source elsewhere in-band drags the
    // walk onto the wrong signal.
    std::size_t        m_dominantAnchorBin;
    std::size_t        m_dominantBandLoBin;
    std::size_t        m_dominantBandHiBin;

    // Per-event gate state — split into two distinct verdicts:
    //   - @c m_provisionalSuppressed drives @c outState.active (the
    //     recorder's fast-drop leading edge). Set by the periodic
    //     provisional gate on partial-event data; can flip either way
    //     during the event as new evidence arrives.
    //   - @c m_gateRejected is the BINDING close-time verdict. It
    //     drives the emitted @c Annotation and the batLike / rejected
    //     event counters the recorder polls at endRecording. Set only
    //     at event close, from the full-event ring.
    // The two are deliberately separate: a provisional reject taken at
    // ~8 ms in (6 hot frames) runs on the loudest frame seen so far —
    // early in an event that is a partially-windowed, attenuated frame,
    // an unreliable basis for a binding verdict. The close-time block
    // must re-examine the full event unconditionally.
    //
    // @c m_framesSinceLastProvisionalEval counts hot frames appended to
    // the dominant-bin ring since the last provisional evaluation. The
    // provisional block fires whenever this reaches GATE_DECISION_FRAMES
    // — the initial fire is natural (starts at 0, ticks up one per hot
    // frame, hits 6 exactly when the ring has 6 hot frames) and every
    // subsequent fire runs on GATE_DECISION_FRAMES of newly-appended
    // ring data. See A3: the previous one-shot latch let a cricket
    // pulse train + 10.7 ms hangover merge into one long suppressed
    // event that swallowed any bat call arriving inside it.
    bool        m_provisionalSuppressed        = false;
    bool        m_gateRejected                 = false;
    std::size_t m_framesSinceLastProvisionalEval = 0;
    // Sidecar diagnostic — true iff this event was rejected by the
    // 6-frame provisional gate (line ~536) as opposed to the full
    // close-time check. Read once at event close, then reset. Not on
    // the RT hot path; touched from the audio thread only, no locking
    // needed.
    bool m_provisionalRejectedThisEvent = false;

    // --- Temporal repetition-rate guard ---
    // Cross-event onset ring: the discriminating signature only emerges
    // over a sequence of events, so this ring persists across event opens
    // and closes (it is reset only on configure()). Fixed capacity, integer
    // indices — no allocation, no locks.
    static constexpr std::size_t ONSET_RING_CAP = 16;
    std::uint32_t m_onsetFrames[ONSET_RING_CAP]{};
    std::size_t   m_onsetCount = 0;    // valid entries (caps at ONSET_RING_CAP)
    std::size_t   m_onsetHead  = 0;    // next write index (circular)

    // Temporal-guard tunables. Defaults calibrated on the shipping-corpus
    // detector output: crickets cluster in the middle of the CV(IDI) axis
    // (roughly 0.6-1.2) with rates 2-15 Hz. Metronomic bat feeding buzzes
    // (e.g. Pipistrellus) sit below that band (CV ~0.2); CF species
    // (Rhinolophus) are similarly regular. Bursty passes (e.g. Barbastella)
    // sit above the band. The veto fires on the *middle* of the CV axis,
    // between rep_cv_min and rep_cv_max — not just "below max".
    // Shipping default is OFF as of the veto-recovery pre-release: on the
    // Pipistrellus corpus the guard was discarding ~2500 real bat calls
    // per night. Re-enable with tunable ``rep_guard_enabled=1`` for
    // deployments in CF-heavy (Rhinolophus/Nyctalus) sites where
    // metronomic-call rejection matters. The gate site is the sole
    // consumer of this flag and both branches are unchanged from prior
    // releases — the veto's decision logic is functionally identical
    // when set back to 1. (Validation observed ~1% event-count drift
    // between the CLI-override path and the code-default path on x86
    // fast-math builds; see CHANGELOG.md.)
    int   m_repGuardEnabled     = 0;
    float m_repRateMinHz        = 1.0f;
    float m_repRateMaxHz        = 20.0f;
    float m_repCvMin            = 0.50f;
    float m_repCvMax            = 1.30f;
    // Onsets required before the temporal verdict is trusted. Two
    // metronomic intervals are enough evidence: on a fresh detector the
    // ring starts empty, so a stricter minimum lets early events through
    // and one passing event is enough for the recorder to keep the clip.
    int   m_repMinEvents        = 2;
    // Bandwidth above which the temporal veto NEVER fires — protects
    // wide-band FM bats (e.g. Myotis) that might occasionally occur in
    // a semi-regular sequence. Genuinely broadband species sweep well
    // above this floor. Retune if a specific site shows a wide-band bat
    // recall regression.
    float m_repBroadbandKeepKhz = 10.0f;

    // Hop size in samples, needed to convert onset frame indices into
    // seconds for the Hz-domain rate bounds. Not part of configure() (that
    // would break the ISweepTracker ABI for older plugins), so DspPipeline
    // sets it as a tunable at start-time. Default matches the shipping
    // production configuration (512 @ 384 kHz → 750 Hz frame rate); the
    // pipeline overrides with the actual hop from DspPipelineConfig.
    int   m_hopSizeSamples      = 512;

    // --- Sidecar diagnostics (mutex-protected, off the audio hot loop) ---
    // The mutex is held twice per event (open + close) plus at drain time,
    // but never per-frame: per-frame span/SNR updates land in the lock-free
    // scratch below and are copied under the lock only at open/close.
    mutable std::mutex          m_diagnosticsMutex;
    EventFeatures               m_inProgressEvent{};
    std::vector<EventFeatures>  m_pendingEvents;
    // Snapshot of m_noiseFloor taken at the open of the FIRST event since the
    // last drain. Empty between drain and the next event-open. The recorder
    // drains both the events vector and this snapshot atomically.
    std::vector<float>          m_floorSnapshotAtFirstEvent;
    std::uint64_t               m_eventsTotalSinceBoot{0};
    std::uint64_t               m_framesProcessedSinceBoot{0};

    // --- Per-event verdict counters (wait-free) ---
    // Incremented exactly once per event at close-time (kept OR rejected).
    // The recorder reads @c m_batLikeEventsSinceBoot via the DSP snapshot to
    // decide whether to keep or discard a just-finished clip. Kept off the
    // diagnostics mutex — the recorder must never wait for the audio thread.
    std::atomic<std::uint64_t>  m_batLikeEventsSinceBoot{0};
    std::atomic<std::uint64_t>  m_rejectedEventsSinceBoot{0};
};
