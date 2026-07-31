// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Stream C poller: polls the DSP tracker's pending-events queue at a
/// fast cadence, dedupes by @c EventFeatures::start_frame, writes one
/// "event" JSONL record per new event to the DecisionLog, and hands the
/// same event off to the EventClipWriter (if set) so Stream B can save
/// the surrounding audio window.
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
#include <unordered_set>

namespace echobox::dsp { class DspPipeline; }

namespace echobox::collection {

class EventClipWriter;

/**
 * @brief Poll interval + dedupe controls for the event poller.
 *
 * The poll cadence must be shorter than the recorder's silence timeout
 * so that events which trigger a WAV are logged BEFORE that WAV closes
 * and drainSidecarPayload() clears the queue. 100 ms is comfortable for
 * the shipped 50 ms silence + hangover budget (events still sit in the
 * queue for at least the silence-timeout window from close).
 */
struct EventPollerConfig {
    std::chrono::milliseconds pollInterval{50};
    std::size_t               dedupeCapacity{4096};   // ~a night's events
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

    // Dedupe set: start_frame values already logged. Bounded — we evict
    // the oldest half when we hit the cap so a long night doesn't leak
    // memory. Collisions across the eviction boundary would double-log
    // (which the offline reader detects and warns about); with hop=512
    // and 30 fps event rate the cap is >1h of headroom before eviction.
    std::unordered_set<std::uint32_t> m_seen;
};

} // namespace echobox::collection
