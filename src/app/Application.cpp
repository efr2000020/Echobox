#include "Application.hpp"

#include "audio/AlsaAudioSource.hpp"
#include "common/PathUtils.hpp"
#include "common/SignalHandler.hpp"
#include "dsp/TrackerRegistry.hpp"
#include "logging/Logger.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>

#ifndef ECHOBOX_DYNAMIC_PLUGINS
// Release builds statically link the detector; its plugin entry points are
// resolved at link time rather than via dlopen.
extern "C" {
    ISweepTracker* create_tracker();
    void           destroy_tracker(ISweepTracker*);
    const char*    get_tracker_name();
}
#endif

namespace echobox::app {

namespace {

constexpr std::size_t kCaptureChunkFrames = 4096;

std::size_t preRollCapacitySamples(const Config& cfg) {
    // preroll + 500 ms safety so a disk-flush hiccup doesn't lose pre-roll data.
    const std::uint64_t ms = static_cast<std::uint64_t>(cfg.preRollMs) + 500;
    return static_cast<std::size_t>(ms * cfg.sampleRate / 1000ULL) * cfg.channels;
}

std::size_t dspRingCapacity(const Config& cfg) {
    // ~10 hops of headroom so the audio thread can burst-push without spinning.
    return cfg.hopSize * 16;
}

} // namespace

Application::Application(Config cfg)
    : m_cfg(std::move(cfg)),
      m_dspRing(dspRingCapacity(m_cfg)),
      m_preRoll(preRollCapacitySamples(m_cfg)) {}

Application::~Application() = default;

void Application::scanPlugins() {
#ifdef ECHOBOX_DYNAMIC_PLUGINS
    fs::path algoPath = PathUtils::getExecutableDir() / "algorithms";
    LS_INFO("app", "scanning plugins in %s", algoPath.string().c_str());
    ::TrackerRegistry::getInstance().scanPlugins(algoPath.string());
#else
    ::TrackerRegistry::getInstance().registerBuiltin(
        create_tracker, destroy_tracker, get_tracker_name);
#endif
    for (const auto& a : ::TrackerRegistry::getInstance().getAvailableAlgorithms()) {
        LS_INFO("registry", "loaded algorithm %s", a.c_str());
    }
}

std::unique_ptr<audio::IAudioSource> Application::makeAudioSource() {
    audio::AlsaAudioSource::Params p;
    p.device     = m_cfg.device;
    p.sampleRate = m_cfg.sampleRate;
    p.channels   = m_cfg.channels;
    return std::make_unique<audio::AlsaAudioSource>(p);
}

int Application::run() {
    common::SignalHandler::install();

    // Logging is opt-in: with --log-level off (the default) we never open a
    // log file, so a shipped unit stays silent and writes nothing to disk.
    if (m_cfg.logLevel != logging::LogLevel::Off) {
        logging::LoggerConfig logCfg;
        logCfg.dir      = m_cfg.logDir;
        logCfg.minLevel = m_cfg.logLevel;
        logging::Logger::instance().start(logCfg);
    }

    LS_INFO("app", "Echobox starting (sr=%d ch=%d alg=%s window=%d-%dHz)",
            m_cfg.sampleRate, m_cfg.channels, m_cfg.algorithm.c_str(),
            m_cfg.freqLoHz, m_cfg.freqHiHz);

    scanPlugins();

    try {
        m_source = makeAudioSource();
        m_source->open();
    } catch (const std::exception& e) {
        LS_ERROR("app", "audio source failed: %s", e.what());
        logging::Logger::instance().stop();
        return 2;
    }

    if (m_source->sampleRate() != m_cfg.sampleRate) {
        LS_WARN("app", "device sample rate %d differs from CLI %d; using device rate",
                m_source->sampleRate(), m_cfg.sampleRate);
        m_cfg.sampleRate = m_source->sampleRate();
    }

    dsp::DspPipelineConfig dcfg;
    dcfg.sampleRate = m_cfg.sampleRate;
    dcfg.fftSize    = m_cfg.fftSize;
    dcfg.hopSize    = m_cfg.hopSize;
    dcfg.freqLoHz   = static_cast<float>(m_cfg.freqLoHz);
    dcfg.freqHiHz   = static_cast<float>(m_cfg.freqHiHz);
    dcfg.algorithm   = m_cfg.algorithm;
    dcfg.sensitivity = m_cfg.sensitivity;
    m_dsp = std::make_unique<dsp::DspPipeline>(dcfg, m_dspRing);

    recorder::RecorderConfig rcfg;
    rcfg.outputDir = m_cfg.outputDir;
    rcfg.sampleRate = m_cfg.sampleRate;
    rcfg.channels   = m_cfg.channels;
    rcfg.preRollMs   = m_cfg.preRollMs;
    rcfg.silenceMs   = m_cfg.silenceMs;
    rcfg.minLengthMs = m_cfg.minLengthMs;
    rcfg.maxLengthMs = m_cfg.maxLengthMs;
    m_recorder = std::make_unique<recorder::Recorder>(rcfg, m_preRoll, *m_dsp);

    try {
        m_dsp->start();
    } catch (const std::exception& e) {
        LS_ERROR("app", "DSP start failed: %s", e.what());
        m_source->close();
        logging::Logger::instance().stop();
        return 3;
    }
    m_recorder->start();

    m_running.store(true, std::memory_order_release);
    m_captureThread = std::thread(&Application::captureLoop, this);

    std::printf("Echobox is running — listening on '%s', saving recordings to '%s'.\n"
                "Press Ctrl+C to stop.\n",
                m_cfg.device.c_str(), m_cfg.outputDir.string().c_str());
    std::fflush(stdout);

    // Main loop: wait for a shutdown signal.
    while (!common::SignalHandler::shouldExit()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LS_INFO("app", "shutdown requested");

    // Stop in reverse order of start.
    m_running.store(false, std::memory_order_release);
    if (m_captureThread.joinable()) m_captureThread.join();

    m_recorder->stop();
    m_dsp->stop();
    m_source->close();

    LS_INFO("app", "stopped");
    logging::Logger::instance().stop();
    std::puts("Echobox stopped.");
    return 0;
}

void Application::captureLoop() {
    std::array<std::int16_t, kCaptureChunkFrames> intBuf{};
    std::array<float, kCaptureChunkFrames>        floatBuf{};
    constexpr float kInvScale = 1.0f / 32768.0f;

    while (m_running.load(std::memory_order_acquire)) {
        int got = m_source->read(std::span<std::int16_t>(intBuf));
        if (got < 0) {
            LS_ERROR("app", "audio source fatal; shutting down");
            common::SignalHandler::requestExit();
            break;
        }
        if (got == 0) continue;

        const std::size_t n = static_cast<std::size_t>(got);

        // 1. Feed the recorder's pre-roll buffer (raw int16, mono).
        m_preRoll.write(std::span<const std::int16_t>(intBuf.data(), n));

        // 2. Convert + push into the DSP ring. Spin briefly on full (the DSP
        //    thread should drain at the audio rate).
        for (std::size_t i = 0; i < n; ++i) {
            floatBuf[i] = static_cast<float>(intBuf[i]) * kInvScale;
        }
        for (std::size_t i = 0; i < n; ++i) {
            while (!m_dspRing.push(floatBuf[i])) {
                if (!m_running.load(std::memory_order_acquire)) return;
                std::this_thread::yield();
            }
        }
    }
}

} // namespace echobox::app
