// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// DSP thread package: input ring → HPF → STFT → tracker plugin →
/// wait-free detector-state snapshot consumed by the recorder.

#include "ISweepTracker.hpp"
#include "core/IFftEngine.hpp"
#include "core/BiquadFilter.hpp"
#include "common/LockFreeRingBuffer.hpp"
#include "recorder/DetectorStateProvider.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace echobox::dsp {

/**
 * @brief Construction-time configuration for DspPipeline.
 *
 * Values are copied into the pipeline at construction and forwarded into the
 * tracker plugin's @c configure() / @c setTunable() when @c start() runs.
 */
struct DspPipelineConfig {
    /// Capture rate in Hz. Drives FFT bin resolution and HPF cutoff math.
    int          sampleRate{384000};
    /// FFT length in samples. Tracker frames carry @c fftSize/2+1 magnitudes.
    std::size_t  fftSize{4096};
    /// FFT hop in samples — also the cadence at which the tracker is invoked.
    std::size_t  hopSize{512};
    /// Lower edge of the detection search window in Hz. HPF cutoff tracks this.
    float        freqLoHz{20000.0f};
    /// Upper edge of the detection search window in Hz. Tracker clamps to Nyquist.
    float        freqHiHz{192000.0f};
    /// Tracker plugin name; resolved through TrackerRegistry at @c start().
    std::string  algorithm{"BandEnergyDetector"};

    /// SNR threshold forwarded to the tracker as the @c band_snr_threshold
    /// tunable after @c configure(). Always applied.
    ///
    /// @note The default below must track @c Config::snrThreshold and the
    ///       @c BandEnergyDetector tunable default — they are linked by
    ///       intent, not by build-time wiring.
    float        snrThreshold{12.0f};
    /// Cricket filter: forwarded to the tracker as the
    /// @c sweep_gate_enabled tunable at start-time. The recorder's
    /// @c cricketDiscard half of the same filter lives in @c RecorderConfig;
    /// @c Application::run flips both from one CLI flag. Trackers that
    /// don't expose the tunable ignore this silently (a plain warn logged
    /// in start()).
    bool         cricketFilter{true};
};

/**
 * @brief Worker that owns the DSP thread and the loaded detector plugin.
 *
 * Consumes float samples from the SPSC ring fed by the audio thread, runs
 * each sample through a Butterworth high-pass biquad, accumulates them into
 * hops, FFTs each hop, and forwards the magnitude frame to a loaded
 * ISweepTracker. Per-frame detector state is published wait-free for the
 * recorder to poll via IDetectorStateProvider.
 *
 * The HPF cutoff is pinned to @c max(20kHz,@c freqLoHz) so audible-band hum
 * can never reach the FFT regardless of the user's chosen detection window.
 *
 * @note Owns exactly one thread (the DSP loop). Non-copyable, non-movable.
 * @note Thread model:
 *       - audio thread writes to the input ring and calls @c notifyInput();
 *       - this class's worker drains the ring and publishes snapshots;
 *       - any thread may call @c snapshot().
 */
class DspPipeline final : public echobox::recorder::IDetectorStateProvider {
public:
    /**
     * @param cfg   Configuration copied into the pipeline; later mutations to
     *              the caller's struct are not observed.
     * @param input Sample ring fed by the audio thread. Lifetime must outlive
     *              this object — the pipeline holds it by reference.
     */
    DspPipeline(DspPipelineConfig cfg, LockFreeRingBuffer<float>& input);
    ~DspPipeline() override;

    DspPipeline(const DspPipeline&)            = delete;
    DspPipeline& operator=(const DspPipeline&) = delete;

    /**
     * @brief Allocate the FFT engine, load the tracker plugin, and spawn the
     *        worker thread. Idempotent on a running pipeline.
     * @throws std::runtime_error if no tracker plugin can be loaded.
     */
    void start();

    /**
     * @brief Stop the worker thread and release the FFT engine / tracker.
     *        Idempotent. Safe to call from any thread.
     */
    void stop();

    /**
     * @brief Producer-side wake signal for the DSP worker.
     *
     * The audio thread calls this once after pushing a batch of samples so
     * the worker — parked on the empty-ring CV — runs as soon as data is
     * available instead of spinning between ALSA bursts.
     *
     * @note Cheap when the worker isn't parked: @c notify_one() on an unwaited
     *       CV is a single atomic.
     */
    void notifyInput();

    /**
     * @brief Wait-free snapshot of the current detector state.
     *
     * Returns the last value published by the worker. Composed from three
     * atomic loads; safe to call from any thread, including a real-time
     * polling loop.
     */
    echobox::recorder::DetectorStateSnapshot snapshot() const override;

    // Sidecar diagnostics. These forward to the loaded tracker; safe to call
    // off the audio hot loop. drainSidecarPayload() and currentTunables()
    // take the tracker's internal lock briefly; the rest are pure accessors.
    bool        drainSidecarPayload(SidecarPayload& out) override;
    bool        currentTunables(std::vector<echobox::recorder::TunableValue>& out) const override;
    std::string algorithmName() const override;
    std::size_t fftSize()  const override { return m_cfg.fftSize; }
    std::size_t hopSize()  const override { return m_cfg.hopSize; }
    float       freqLoHz() const override { return m_cfg.freqLoHz; }
    float       freqHiHz() const override { return m_cfg.freqHiHz; }

private:
    void loop();
    void publish(bool active, float loHz, float hiHz,
                 std::uint64_t batLikeEvents);

    DspPipelineConfig             m_cfg;
    LockFreeRingBuffer<float>&    m_input;

    std::unique_ptr<IFftEngine>   m_fft;
    BiquadFilter                  m_hpf;
    std::unique_ptr<ISweepTracker, std::function<void(ISweepTracker*)>> m_tracker;

    std::atomic<bool>             m_running{false};
    std::thread                   m_thread;

    // Park / wake the DSP thread when the input ring is empty. ALSA delivers
    // samples in ~10 ms bursts; without this the thread spins for the whole
    // inter-burst gap — a battery killer on a Pi Zero 2 W.
    std::mutex                    m_wakeMutex;
    std::condition_variable       m_wakeCv;

    // Wait-free state snapshot consumed by the recorder thread. lo/hi are
    // packed into one 64-bit atomic so the reader can't observe a torn
    // (lo from update N, hi from update N+1) pair on a band-edge change.
    std::atomic<bool>             m_active{false};
    std::atomic<std::uint64_t>    m_loHiBits{0};
    std::atomic<std::uint64_t>    m_generation{0};
    // Monotonic count of kept (non-gate-rejected) events, re-read each
    // publish() from the tracker's own wait-free counter. The recorder
    // snapshots this on begin/end to discard clips with no bat-like event.
    std::atomic<std::uint64_t>    m_batLikeEvents{0};
};

} // namespace echobox::dsp
