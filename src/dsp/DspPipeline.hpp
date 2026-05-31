#pragma once
#include "ISweepTracker.hpp"
#include "core/IFftEngine.hpp"
#include "core/BiquadFilter.hpp"
#include "common/LockFreeRingBuffer.hpp"
#include "recorder/DetectorStateProvider.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace echobox::dsp {

struct DspPipelineConfig {
    int          sampleRate{384000};
    std::size_t  fftSize{4096};
    std::size_t  hopSize{512};
    float        freqLoHz{20000.0f};
    float        freqHiHz{192000.0f};
    std::string  algorithm{"BandEnergyDetector"};
    // EXPERIMENTAL: coarse preset name forwarded to ISweepTracker::applyPreset
    // after configure(). Empty = leave the tracker at compiled defaults.
    std::string  sensitivity{};
};

/**
 * The DSP thread, packaged.
 *
 * Owns the FFT engine, the high-pass biquad and the loaded ISweepTracker
 * plugin. Pops floats from an input ring buffer (fed by the audio thread),
 * runs HPF → STFT → tracker, and publishes a wait-free per-frame
 * DetectorStateSnapshot so the Recorder can react on the leading edge.
 *
 * The tracker is loaded via the existing plugin registry, which scans .so
 * files at startup. The freqLoHz/Hi values are forwarded into the tracker's
 * configure() so it can shape its detection bands to the user-requested
 * window. The HPF cutoff tracks freqLoHz so the biquad helps with the same
 * window.
 */
class DspPipeline final : public echobox::recorder::IDetectorStateProvider {
public:
    DspPipeline(DspPipelineConfig cfg,
                LockFreeRingBuffer<float>& input);
    ~DspPipeline() override;

    DspPipeline(const DspPipeline&)            = delete;
    DspPipeline& operator=(const DspPipeline&) = delete;

    void start();
    void stop();

    // IDetectorStateProvider
    echobox::recorder::DetectorStateSnapshot snapshot() const override;

private:
    void loop();
    void publish(bool active, float loHz, float hiHz);

    DspPipelineConfig             m_cfg;
    LockFreeRingBuffer<float>&    m_input;

    std::unique_ptr<IFftEngine>   m_fft;
    BiquadFilter                  m_hpf;
    std::unique_ptr<ISweepTracker, std::function<void(ISweepTracker*)>> m_tracker;

    std::atomic<bool>             m_running{false};
    std::thread                   m_thread;

    // Wait-free state snapshot for the recorder thread.
    std::atomic<bool>             m_active{false};
    std::atomic<std::uint64_t>    m_loHiBits{0};
    std::atomic<std::uint64_t>    m_generation{0};
};

} // namespace echobox::dsp
