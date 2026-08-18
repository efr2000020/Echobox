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
    bool          drainCollectionEvents(std::vector<EventFeatures>& out) override;
    bool          seedNoiseFloor(std::span<const float> floor) override;
    std::uint64_t totalEventsSinceBoot() const override;

    /**
     * @brief Wait-free counter of events kept by the noise gate since boot.
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
    /// Counter of events rejected by the noise gate. Debugging convenience;
    /// wait-free like @c batLikeEventsSinceBoot().
    std::uint64_t rejectedEventsSinceBoot() const {
        return m_rejectedEventsSinceBoot.load(std::memory_order_relaxed);
    }

    // --- Sweep-shape feature helper (diagnostics) ---
    // These four features no longer decide anything: they were measured
    // non-discriminative on the field corpus (AUC 0.37-0.56, see the
    // m_noiseRejectEnabled block below) and the verdict built on them was
    // retired. They are still computed once per event and written to the
    // sidecar, because offline tooling reasons about them and because a
    // future gate redesign needs them recoverable from shipped captures.
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
    // Top-K mean band SNR a frame must clear to count as hot. The primary
    // sensitivity knob, and monotone across the whole measured range.
    // Deterministic offline harness (production DSP, no recorder, no
    // threads), gate disabled, 24 stratified bat-positive files per
    // session, per-call recall against BatDetect2 proxy truth:
    //
    //   snr 20, flat 0.95   80.80 % / 85.10 %   duty 10.9 / 11.1 %
    //   snr 16, flat 0.95   86.24 % / 89.92 %   duty 12.5 / 12.8 %
    //   snr 12, flat 0.80   91.66 % / 94.04 %   duty 14.8 / 15.0 %
    //   snr  8, flat 0.80   97.43 % / 97.93 %   duty 19.1 / 19.0 %
    //                                    (session_01 / session_02)
    //
    // Lowered 12.0 -> 8.0: +5.8 / +3.9 pp per-call recall for +4.3 / +4.0
    // pp detector duty cycle. Below 8 the curve saturates (~98 % at snr 6)
    // while duty keeps climbing, so 8.0 is the last step that still buys
    // recall per byte. Duty cycle is the cost side of this knob — it sets
    // how much audio the recorder is asked to write per night, which on a
    // solar-powered Pi Zero 2 W is the binding constraint. Anything that
    // raises this also has to be affordable in MB/night.
    float m_bandSnrThreshold  = 8.0f;

    float m_minFlatness       = 0.10f;
    // Spectral-flatness upper bound; rejects broadband transients (clicks,
    // rustling, mechanical impacts) that lift every bin's SNR at once.
    // Same harness as above, holding snr at 12:
    //
    //   flat 0.65           85.88 % / 90.77 %   duty 13.8 / 14.3 %
    //   flat 0.80           91.66 % / 94.04 %   duty 14.8 / 15.0 %
    //   flat 0.90           91.66 % / 94.04 %   duty 14.8 / 15.0 %
    //   flat 1.00 (off)     91.67 % / 94.04 %   duty 14.8 / 15.0 %
    //
    // Raised 0.65 -> 0.80: the entire effect lands in that one step, and
    // 0.90 / 1.00 measure identically to 0.80 — no real bat frame in this
    // corpus has flatness above 0.80. So 0.65 was cutting into the bat
    // population while buying no rejection that 0.80 doesn't already buy.
    //
    // The bound is kept rather than removed because its job is to reject
    // broadband transients (clicks, rustling, mechanical impacts) that
    // this Pipistrellus-only reference corpus happens not to contain.
    // The corpus can show that 0.80 costs no recall; it cannot show the
    // bound is unnecessary. Turning it off entirely (1.00) moved recall
    // by 0.01 pp — one call in 8622 — which is evidence for neither.
    float m_maxFlatness       = 0.80f;

    int   m_warmupFramesLimit = 40;
    int   m_minActiveFrames   = 2;
    int   m_hangoverFrames    = 8;

    // --- Confident-reject noise gate (cricket / false-positive rejection) ---
    // Master switch: 1 = the reject rule is armed. When 0, both
    // outState.active and the closed-event Annotation are byte-identical to
    // what the detector would emit with no gate at all, so an operator can
    // turn the whole filter off with a single tunable.
    //
    // DIRECTION. This gate rejects only what it can positively identify as
    // NOT a bat, and keeps everything it is unsure about. Its predecessor
    // (the sweep-shape gate) did the opposite — it kept only what it could
    // confirm was a bat — and that cost 63 pp of per-call recall, because
    // the four sweep features carry no signal for the species in this
    // corpus. Measured rank-order separation between bat-positive and
    // non-bat detector events (AUC; 0.5 = chance), 82 491 labelled events:
    //
    //     bandwidth_khz  0.556      trigger_snr       0.781
    //     drift_khz      0.518      trigger_flatness  0.262 (inverted 0.738)
    //     path_ratio     0.467      lo_hz / band      0.252 (inverted 0.748)
    //     mono_fraction  0.366 (inverted)
    //
    // Only the right-hand column separates, and none of it was used by the
    // old verdict. The three features below are exactly that column.
    //
    // THE RULE. Reject an event iff ALL of:
    //     trigger_snr      <  m_noiseSnrMax        (weak)
    //     trigger_flatness >  m_noiseFlatnessMin   (broadband)
    //     band_index       <= m_noiseBandMax       (low, 20-45 kHz)
    // Conjunction, not disjunction: any one feature looking bat-like is
    // enough to keep the event. All three are read at event OPEN (see
    // m_noiseRejectThisEvent) — there is no close-time-only term, which is
    // why this gate needs no provisional/binding split.
    //
    // DEFAULTS. 120-file probe per session, per-call recall against
    // BatDetect2 proxy truth, at the shipped snr 8 / flat 0.80 detector:
    //
    //     rule                                s01      s02    non-bat removed
    //     gate OFF                          98.38 %  99.06 %      0 %  /  0 %
    //     old sweep gate                    37.49 %  35.87 %   79.6 % / 76.2 %
    //     snr<12 & flat>0.55 & band1        96.04 %  97.66 %   41.4 % / 25.6 %
    //     snr<15 & flat>0.55 & band1        94.14 %  96.50 %   47.1 % / 29.7 %
    //     band1 & snr<15                    94.07 %  96.45 %   54.4 % / 34.2 %
    //
    // 12 / 0.55 / 1 is the recommended row: worst-case 96.0 % recall — a
    // 1-2 pp cost against gate-off, versus the old gate's 63 pp — while
    // removing a quarter to two-fifths of non-bat clips. The more
    // aggressive rows are reachable at runtime, without a rebuild, for a
    // site that would rather spend recall on bytes.
    //
    // WHAT THIS FILTER IS. Validated as a WEAK-BROADBAND-TRIGGER filter,
    // not as a proven tonal-cricket rejector. The non-bat population on
    // these two nights has *high* spectral flatness (median 0.63) — it is
    // broadband noise admitted by lowering band_snr_threshold to 8, not
    // tonal cricket harmonics. Neither session_01 nor session_02 contains
    // a cricket-dominated stretch, so this corpus cannot demonstrate
    // tonal-cricket rejection either way. Validating that needs a night
    // where crickets actually dominate.
    // See private_docs/audits/01_per_call_recall_audit.md §7b.
    int   m_noiseRejectEnabled = 1;
    float m_noiseSnrMax        = 12.0f;  // reject only below this trigger SNR
    float m_noiseFlatnessMin   = 0.55f;  //   AND above this spectral flatness
    int   m_noiseBandMax       = 1;      //   AND at or below this sub-band (1-based)

    // --- Sweep-shape features: DIAGNOSTICS ONLY ---
    // The four sweep features are still computed and written to the sidecar
    // (offline tooling in tools/session_screen reasons about them, and the
    // recorder's --save-rejected boundary mode reads min_bandwidth_khz out
    // of the tunable manifest), and sweep_bat_like still records what the
    // retired verdict WOULD have said so gate-redesign A/Bs stay possible
    // from a shipped sidecar. Nothing here decides anything any more,
    // except that m_sweepDriftKhz / m_sweepPathRatioMax / m_sweepMonoFracMin
    // are also the "clear bat" bypass of the temporal rep-guard below.
    float m_minBandwidthKhz   = 0.9f;   // 10-dB BW the retired verdict wanted
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

    // Per-event verdict — ONE verdict, taken once, at event open.
    //
    // All three reject features (trigger SNR, trigger flatness, band
    // index) are final the instant the event opens; none of them improves
    // with more of the event in hand. So there is nothing for a
    // close-time re-evaluation to learn, and the provisional/binding
    // split the sweep gate needed does not exist here.
    //
    // That split is deliberately gone rather than merely unused. It
    // existed because a sweep-shape verdict taken at ~8 ms ran on a
    // partially-windowed, attenuated snapshot frame and could not be
    // trusted, so a second, binding pass had to re-examine the full
    // event — and keeping two verdicts in sync produced three separate
    // correctness bugs in the 0.4.0 cycle (A2: the provisional flag
    // vetoed the close-time gate; A3: it latched, so one wrong reject at
    // 8 ms suppressed the rest of the event including any bat call
    // inside it; A7: the sidecar mislabelled re-opened events). All
    // three were bugs in the machinery, not in the thresholds.
    //
    // Consequence for @c outState.active: the filter no longer touches
    // it. A rejected event is recorded normally and then dropped by the
    // recorder at endRecording, via the batLike counter contract below
    // (§12 of DSP_PIPELINE.md). Same bytes on the card either way — the
    // clip is deleted, not kept — and it means the shipped behaviour is
    // exactly the per-clip behaviour §7b.6 measured, rather than a
    // fast-drop variant whose clip geometry was never measured.
    bool m_noiseRejectThisEvent = false;

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
    // Second, collection-only mirror of m_pendingEvents. Populated at the
    // same point under the same lock, but drained by the collection
    // overlay's Stream C poller and never touched by drainSidecarPayload.
    // Lets that poller pull events destructively at its own cadence
    // without racing the recorder's per-clip drain, which would otherwise
    // swallow events that opened and closed between two poller wake-ups.
    // Stays empty in the shipping unit: nothing drains it unless
    // --collection-mode on, and nothing pushes to it either (see the
    // m_collectionEventsWanted gate below).
    std::vector<EventFeatures>  m_collectionEvents;
    // Kill switch for the mirror. False in the shipping unit, so event
    // close does not push a second copy and the vector never allocates.
    // Latched true by the first drainCollectionEvents() call — i.e. only
    // once the collection poller exists, which only happens under
    // --collection-mode on.
    bool                        m_collectionEventsWanted{false};
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
