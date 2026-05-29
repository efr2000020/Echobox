#pragma once
#include "../../ISweepTracker.hpp"  // src/dsp/algorithms/BandEnergyDetector/ -> src/dsp/
#include <span>
#include <cstdint>
#include <vector>

/**
 * Wide-band bat PRESENCE detector.
 *
 * The detector integrates energy across several sub-bands spanning the
 * configured detection window. A frame is "active" if ANY sub-band's energy
 * rises sufficiently above that sub-band's own adaptive noise floor.
 *
 * Why this design:
 *  - Steep-FM bats (e.g. Myotis) sweep across 100+ kHz within a single FFT
 *    frame. A single-peak search picks one bin and discards the rest of the
 *    sweep, and tends to lock onto the loud low-frequency tail — so faint
 *    high-frequency calls get masked. Energy integration has no "winner": a
 *    high-frequency sweep contributes its energy and is judged on its own merit.
 *  - Per-sub-band floors mean each frequency region gets a fair, level-
 *    appropriate threshold. High-frequency calls are physically fainter
 *    (atmospheric absorption rises with frequency) and sit on a much lower
 *    noise floor; a per-band SNR ratio handles both automatically, where a
 *    single absolute threshold could not.
 *
 * The detector emits the standard Annotation (start/end frame + freq span of
 * the firing sub-band) when an event closes, and publishes a per-frame
 * DetectorState so the downstream recorder can react on the leading edge.
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
        20000.0f, 45000.0f, 80000.0f, 130000.0f, 190000.0f
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

    // Diagnostics
    int   m_frameCount;
    float m_maxBandSnrInHeartbeat;
    int   m_peakBandInHeartbeat;

    // --- Tuning parameters (validated against real recordings; see project HANDOFF) ---
    static constexpr float ALPHA_RISE = 0.995f;
    static constexpr float ALPHA_FALL = 0.90f;
    static constexpr float MIN_ABS_FLOOR = 1e-6f;

    static constexpr int   TOP_K = 8;
    static constexpr float BAND_SNR_THRESHOLD = 12.0f;

    static constexpr float MIN_FLATNESS = 0.10f;
    static constexpr float MAX_FLATNESS = 0.75f;

    static constexpr int WARMUP_FRAMES     = 40;
    static constexpr int MIN_ACTIVE_FRAMES = 2;
    static constexpr int HANGOVER_FRAMES   = 8;
};
