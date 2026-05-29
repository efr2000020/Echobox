#include "BandEnergyDetector.hpp"
#include "logging/Logger.hpp"

#include <algorithm>
#include <cmath>

constexpr float BandEnergyDetector::BAND_EDGES_HZ[];

BandEnergyDetector::BandEnergyDetector()
    : m_sampleRate(0), m_fftSize(0), m_binResolution(0.0f),
      m_freqLoHz(0.0f), m_freqHiHz(0.0f),
      m_inBandLo(0), m_inBandHi(0),
      m_floorSeeded(false), m_warmupFrames(0),
      m_inEvent(false), m_activeRun(0), m_silenceFrames(0),
      m_eventStart(0), m_eventLoHz(0.0f), m_eventHiHz(0.0f),
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
    m_frameCount            = 0;
    m_maxBandSnrInHeartbeat = 0.0f;
    m_peakBandInHeartbeat   = -1;

    LS_INFO("dsp.bed", "configured: sr=%d fft=%zu res=%.1fHz bands=%zu window=%.0f-%.0fHz",
            sampleRate, fftSize, m_binResolution, m_bands.size(), userLo, userHi);
}

bool BandEnergyDetector::processFrame(std::span<const float> magnitudes,
                                      std::uint32_t currentFrame,
                                      DetectorState& outState,
                                      Annotation& outAnnotation) {
    ++m_frameCount;
    ++m_warmupFrames;

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
            const float floor = (m_noiseFloor[i] > MIN_ABS_FLOOR) ? m_noiseFloor[i] : MIN_ABS_FLOOR;
            m_bandSnrScratch[i] = magnitudes[i] / floor;
        }

        const int k = static_cast<int>(std::min(static_cast<std::size_t>(TOP_K), count));
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
        const float alpha = (mag > floor) ? ALPHA_RISE : ALPHA_FALL;
        m_noiseFloor[i] = alpha * floor + (1.0f - alpha) * mag;
    }

    const bool warmedUp = (m_warmupFrames >= WARMUP_FRAMES);

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
    const bool flatnessOk = (flatness > MIN_FLATNESS) && (flatness < MAX_FLATNESS);

    const bool hot = warmedUp && (bestBand >= 0)
                     && (bestBandSnr > BAND_SNR_THRESHOLD)
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

        if (!m_inEvent && m_activeRun >= MIN_ACTIVE_FRAMES) {
            m_inEvent    = true;
            m_eventStart = currentFrame - static_cast<std::uint32_t>(m_activeRun - 1);
            m_eventLoHz  = bandLoHz;
            m_eventHiHz  = bandHiHz;
            LS_INFO("dsp.bed", "event start frame=%u snr=%.2f", m_eventStart, bestBandSnr);
        }
        if (m_inEvent) {
            m_eventLoHz = std::min(m_eventLoHz, bandLoHz);
            m_eventHiHz = std::max(m_eventHiHz, bandHiHz);
        }
    } else {
        if (m_inEvent) {
            ++m_silenceFrames;
            if (m_silenceFrames > HANGOVER_FRAMES) {
                outAnnotation.start_frame = m_eventStart;
                outAnnotation.end_frame   = currentFrame - static_cast<std::uint32_t>(m_silenceFrames);
                outAnnotation.low_freq    = m_eventLoHz;
                outAnnotation.high_freq   = m_eventHiHz;

                LS_INFO("dsp.bed", "event end frames=%u-%u %.1f-%.1fkHz",
                        m_eventStart, outAnnotation.end_frame,
                        m_eventLoHz / 1000.0f, m_eventHiHz / 1000.0f);

                m_inEvent          = false;
                m_activeRun        = 0;
                m_silenceFrames    = 0;
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

// --- Plugin entry points (resolved by TrackerRegistry via dlsym) ---
extern "C" {
    ISweepTracker* create_tracker()                    { return new BandEnergyDetector(); }
    void           destroy_tracker(ISweepTracker* t)   { delete static_cast<BandEnergyDetector*>(t); }
    const char*    get_tracker_name()                  { return "BandEnergyDetector"; }
}
