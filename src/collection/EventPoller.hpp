// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Stream C poller: destructively drains the DSP tracker's collection-
/// only events queue at a fast cadence, writes one "event" JSONL record
/// per event to the DecisionLog, and hands the same event off to the
/// EventClipWriter (if set) so Stream B can save the surrounding audio
/// window.
///
/// The tracker maintains a second queue populated in parallel with the
/// sidecar queue (see @c ISweepTracker::drainCollectionEvents), so this
/// poller can pull events without racing the recorder's per-clip
/// @c drainSidecarPayload — a non-destructive peek + start_frame dedupe
/// silently loses events whose entire lifetime fits between two poller
/// wake-ups.
///
/// The tracker's mirror queue stays cold — and therefore free for the
/// shipping unit — until something drains it once. That arming drain is
/// issued by @c DspPipeline::start() under
/// @c DspPipelineConfig::collectEvents, not by this class: the tracker
/// does not exist yet when this poller is started.
///
/// Runs on its own thread so the DSP hot loop is untouched by
/// collection concerns. See DATA_COLLECTION_IMPL_VALIDATION_PLAN §2.2 C.

#include "DecisionLog.hpp"

#include "dsp/ISweepTracker.hpp"   // EventFeatures

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace echobox::dsp { class DspPipeline; }

namespace echobox::collection {

class EventClipWriter;

/**
 * @brief Poll interval for the event poller.
 *
 * Cadence primarily controls end-to-end latency between an event closing
 * on the DSP thread and its WAV landing in @c events/. A tighter cadence
 * also shortens the window over which Stream B's preroll buffer must
 * hold audio waiting for a clip to be written.
 */
struct EventPollerConfig {
    std::chrono::milliseconds pollInterval{50};
    std::size_t               hopSize{512};           // frame→sample conversion
    int                       sampleRate{384000};
};

class EventPoller {
public:
    EventPoller(EventPollerConfig cfg,
                echobox::dsp::DspPipeline& pipeline,
                DecisionLog& log);
    ~EventPoller();

    EventPoller(const EventPoller&)            = delete;
    EventPoller& operator=(const EventPoller&) = delete;

    /// Optional: forward each new event to a clip-writer so Stream B
    /// can save the audio window around it. Null ⇒ Stream B is off.
    /// Non-owning; the writer's lifetime must exceed the poller's.
    void setClipWriter(EventClipWriter* w) { m_clipWriter = w; }

    void start();
    void stop();

    std::uint64_t eventsLogged() const {
        return m_eventsLogged.load(std::memory_order_acquire);
    }

private:
    void loop();
    void handleNew(const EventFeatures& e);
    std::string encodeEventJson(const EventFeatures& e,
                                std::uint64_t start_sample,
                                std::uint64_t end_sample) const;

    EventPollerConfig       m_cfg;
    echobox::dsp::DspPipeline& m_pipeline;
    DecisionLog&            m_log;
    EventClipWriter*        m_clipWriter{nullptr};

    std::atomic<bool>       m_running{false};
    std::thread             m_thread;
    std::atomic<std::uint64_t> m_eventsLogged{0};
};

} // namespace echobox::collection
