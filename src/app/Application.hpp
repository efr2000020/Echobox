#pragma once
#include "Config.hpp"
#include "audio/IAudioSource.hpp"
#include "common/LockFreeRingBuffer.hpp"
#include "dsp/DspPipeline.hpp"
#include "recorder/PreRollBuffer.hpp"
#include "recorder/Recorder.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace litespec::app {

/**
 * Top-level wiring + lifecycle.
 *
 * Owns one audio source, the FFT/detector DSP pipeline, the recorder, and the
 * logger configuration. Constructs them in dependency order, runs the audio
 * capture loop on its own thread, and tears everything down in reverse order
 * on exit.
 */
class Application {
public:
    explicit Application(Config cfg);
    ~Application();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    /** Blocks until SIGINT/SIGTERM or a fatal error. Returns exit code. */
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
};

} // namespace litespec::app
