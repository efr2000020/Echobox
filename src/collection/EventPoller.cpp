// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// EventPoller implementation. See EventPoller.hpp for the contract.

#include "EventPoller.hpp"

#include "EventClipWriter.hpp"
#include "dsp/DspPipeline.hpp"
#include "logging/Logger.hpp"

#include <sstream>
#include <vector>

namespace echobox::collection {

EventPoller::EventPoller(EventPollerConfig cfg,
                         echobox::dsp::DspPipeline& pipeline,
                         DecisionLog& log)
    : m_cfg(std::move(cfg)), m_pipeline(pipeline), m_log(log) {}

EventPoller::~EventPoller() { stop(); }

void EventPoller::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;
    m_seen.reserve(m_cfg.dedupeCapacity);
    m_thread = std::thread(&EventPoller::loop, this);
}

void EventPoller::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    if (m_thread.joinable()) m_thread.join();
    LS_INFO("collection",
            "STREAM_C_STOP events_logged=%llu",
            static_cast<unsigned long long>(m_eventsLogged.load()));
}

std::string EventPoller::encodeEventJson(const EventFeatures& e,
                                         std::uint64_t start_sample,
                                         std::uint64_t end_sample) const {
    std::ostringstream oss;
    oss << "{\"kind\":\"event\","
        << "\"start_sample\":" << start_sample                    << ","
        << "\"end_sample\":"   << end_sample                      << ","
        << "\"start_frame\":"  << e.start_frame                   << ","
        << "\"end_frame\":"    << e.end_frame                     << ","
        << "\"duration_frames\":" << e.duration_frames            << ","
        << "\"band_index\":"   << e.band_index                    << ","
        << "\"trigger_snr\":"  << e.trigger_snr                   << ","
        << "\"trigger_flatness\":" << e.trigger_flatness          << ","
        << "\"peak_snr\":"     << e.peak_snr                      << ","
        << "\"lo_hz\":"        << e.lo_hz                         << ","
        << "\"hi_hz\":"        << e.hi_hz                         << ","
        << "\"bandwidth_khz\":" << e.bandwidth_khz                << ","
        << "\"drift_khz\":"    << e.drift_khz                     << ","
        << "\"path_ratio\":"   << e.path_ratio                    << ","
        << "\"mono_fraction\":" << e.mono_fraction                << ","
        << "\"gate_rejected\":" << (e.gate_rejected ? "true" : "false")
        << "}";
    return oss.str();
}

void EventPoller::handleNew(const EventFeatures& e) {
    const std::uint64_t start_sample =
        static_cast<std::uint64_t>(e.start_frame)
        * static_cast<std::uint64_t>(m_cfg.hopSize);
    // end_frame is inclusive per the tracker's own convention; add +1 so
    // the sample span matches recorder_model.py's (start, end+1) window.
    const std::uint64_t end_sample =
        (static_cast<std::uint64_t>(e.end_frame) + 1ULL)
        * static_cast<std::uint64_t>(m_cfg.hopSize);

    m_log.append(encodeEventJson(e, start_sample, end_sample));
    m_eventsLogged.fetch_add(1, std::memory_order_release);

    if (m_clipWriter) {
        m_clipWriter->enqueue(e, start_sample, end_sample);
    }
}

void EventPoller::loop() {
    std::vector<EventFeatures> scratch;
    scratch.reserve(128);

    while (m_running.load(std::memory_order_acquire)) {
        // Sleep in slices so stop() joins within a few ms rather than
        // waiting a full poll interval.
        const auto sliceMs = std::chrono::milliseconds(20);
        auto waited = std::chrono::milliseconds(0);
        while (waited < m_cfg.pollInterval
               && m_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(sliceMs);
            waited += sliceMs;
        }
        if (!m_running.load(std::memory_order_acquire)) break;

        scratch.clear();
        if (!m_pipeline.peekPendingEvents(scratch)) continue;

        for (const auto& e : scratch) {
            // Eviction: keep unbounded growth in check without corrupting
            // dedupe within a night's worth of events. Drop the whole
            // set at the cap; the tracker's own queue is drained by the
            // recorder often enough that a repeat start_frame across the
            // eviction is unlikely (and offline readers detect it).
            if (m_seen.size() >= m_cfg.dedupeCapacity) {
                m_seen.clear();
                m_seen.reserve(m_cfg.dedupeCapacity);
            }
            if (m_seen.insert(e.start_frame).second) {
                handleNew(e);
            }
        }
    }
    m_log.flush();
}

} // namespace echobox::collection
