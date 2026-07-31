// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Top-level glue: owns one audio source, the DSP pipeline, the recorder,
/// and the capture thread. Construction is cheap; @c run() wires everything
/// up and blocks until shutdown.

#include "Config.hpp"
#include "audio/IAudioSource.hpp"
#include "collection/ContinuousWriter.hpp"
#include "collection/DecisionLog.hpp"
#include "collection/EventClipWriter.hpp"
#include "collection/EventPoller.hpp"
#include "collection/RecorderDecisionSink.hpp"
#include "collection/ReferenceRing.hpp"
#include "collection/SampleClock.hpp"
#include "collection/Session.hpp"
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

    // Data-collection overlay. m_sampleClock is a tiny atomic uint64 that
    // is only advanced when m_cfg.collection.enabled is true — the audio
    // hot loop skips the fetch_add otherwise, so the shipping-off path is
    // byte-identical to R2v3. Everything else is null when the overlay is
    // off (the ring/writer are Stream-A-only; a small-card deployment can
    // opt out via --collection-streams).
    ::echobox::collection::SampleClock                     m_sampleClock;
    std::unique_ptr<::echobox::collection::Session>        m_session;
    std::unique_ptr<::echobox::collection::ReferenceRing>  m_referenceRing;
    std::unique_ptr<::echobox::collection::ContinuousWriter> m_continuousWriter;
    // Stream B: dedicated preroll buffer for event-clip windows. Sized
    // larger than the recorder's own preroll so pre+post windows fit.
    std::unique_ptr<recorder::PreRollBuffer>                 m_collectionPreRoll;
    std::unique_ptr<::echobox::collection::DecisionLog>      m_decisionLog;
    std::unique_ptr<::echobox::collection::EventPoller>      m_eventPoller;
    std::unique_ptr<::echobox::collection::EventClipWriter>  m_eventClipWriter;
    // Sink adapter: forwards Recorder decisions to the decision log as
    // "decision" JSONL records. Lives here so its lifetime tracks the
    // Application (not the Recorder — which sees it only through a raw
    // interface pointer).
    class RecorderDecisionForwarder;
    std::unique_ptr<RecorderDecisionForwarder>               m_decisionForwarder;
    // Rate-limited drop warning so a sustained ring-full doesn't flood the
    // op log; the exact drop count is always available on the ReferenceRing.
    std::chrono::steady_clock::time_point m_lastRefDropWarnAt{};

    std::atomic<bool> m_running{false};
    std::thread       m_captureThread;

    // Audio thread is best-effort into the DSP ring: never blocks the ALSA
    // reader. Drops are counted and surfaced via a rate-limited warning so a
    // sustained DSP stall is visible without flooding the log.
    std::uint64_t                         m_dspDroppedTotal{0};
    std::chrono::steady_clock::time_point m_lastDropWarnAt{};
};

} // namespace echobox::app
