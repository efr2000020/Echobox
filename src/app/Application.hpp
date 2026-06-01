#pragma once
/// @file
/// Top-level glue: owns one audio source, the DSP pipeline, the recorder,
/// and the capture thread. Construction is cheap; @c run() wires everything
/// up and blocks until shutdown.

#include "Config.hpp"
#include "audio/IAudioSource.hpp"
#include "common/LockFreeRingBuffer.hpp"
#include "dsp/DspPipeline.hpp"
#include "recorder/PreRollBuffer.hpp"
#include "recorder/Recorder.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

namespace echobox::app {

/**
 * @brief Top-level wiring and process lifecycle.
 *
 * Owns the audio source, the FFT/detector DSP pipeline, the recorder, and
 * the logger configuration. Subsystems are constructed in dependency order,
 * the audio capture loop runs on its own thread, and teardown happens in
 * reverse order on exit.
 *
 * @note Owns the capture thread. Non-copyable.
 */
class Application {
public:
    /// @param cfg Configuration consumed (moved-from) by the constructor.
    explicit Application(Config cfg);
    ~Application();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    /**
     * @brief Run the application until shutdown is requested.
     *
     * Opens the audio device, starts the DSP and recorder threads, then
     * blocks the calling thread on a poll of @c SignalHandler::shouldExit
     * until SIGINT/SIGTERM arrives or a fatal subsystem error occurs.
     *
     * @return Process exit code: @c 0 normal, @c 2 audio source failure,
     *         @c 3 DSP start failure.
     */
    int run();

private:
    void captureLoop();
    void scanPlugins();
    std::unique_ptr<audio::IAudioSource> makeAudioSource();

    Config m_cfg;

    LockFreeRingBuffer<float>            m_dspRing;
    recorder::PreRollBuffer              m_preRoll;

    std::unique_ptr<audio::IAudioSource>   m_source;
    std::unique_ptr<dsp::DspPipeline>      m_dsp;
    std::unique_ptr<recorder::Recorder>    m_recorder;

    std::atomic<bool> m_running{false};
    std::thread       m_captureThread;

    // Audio thread is best-effort into the DSP ring: never blocks the ALSA
    // reader. Drops are counted and surfaced via a rate-limited warning so a
    // sustained DSP stall is visible without flooding the log.
    std::uint64_t                         m_dspDroppedTotal{0};
    std::chrono::steady_clock::time_point m_lastDropWarnAt{};
};

} // namespace echobox::app
