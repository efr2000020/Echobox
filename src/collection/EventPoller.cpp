// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// EventPoller implementation. See EventPoller.hpp for the contract.

#include "EventPoller.hpp"

#include "EventClipWriter.hpp"
#include "dsp/DspPipeline.hpp"
#include "logging/Logger.hpp"
#include "recorder/Sidecar.hpp"    // base64EncodeFloats

#include <algorithm>
#include <sstream>
#include <vector>

namespace echobox::collection {

EventPoller::EventPoller(EventPollerConfig cfg,
                         echobox::dsp::DspPipeline& pipeline,
                         DecisionLog& log,
                         const SampleClock& clock)
    : m_cfg(std::move(cfg)), m_pipeline(pipeline), m_log(log), m_clock(clock) {}

EventPoller::~EventPoller() { stop(); }

void EventPoller::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;
    // No arming call here: the tracker does not exist until
    // DspPipeline::start() creates it, which happens after this. Arming
    // is a DspPipelineConfig::collectEvents concern, applied at the point
    // the tracker is constructed — see DspPipeline::start().
    m_thread = std::thread(&EventPoller::loop, this);
}

void EventPoller::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    if (m_thread.joinable()) m_thread.join();
    LS_INFO("collection",
            "STREAM_C_STOP events_logged=%llu noise_floors_logged=%llu",
            static_cast<unsigned long long>(m_eventsLogged.load()),
            static_cast<unsigned long long>(m_floorsLogged.load()));
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
        << "\"gate_rejected\":" << (e.gate_rejected ? "true" : "false") << ","
        // --- decision-path diagnostics ---
        // These six fields did not exist when this overlay was first
        // written; the gate they describe replaced the sweep-shape gate
        // it was built against. gate_rejected is still the binding
        // verdict, but the four sweep-shape features above are now pure
        // diagnostics, and WHICH clause fired is only recoverable from
        // this block: veto_applied distinguishes a temporal rep-guard
        // reject from a confident-reject noise-rule reject, which is
        // exactly the "which clause fired" the plan's §2.2 C asks for.
        // Logging the stale field set instead would have made Stream C
        // unable to explain any rejection this firmware makes.
        << "\"sweep_bat_like\":" << (e.sweep_bat_like ? "true" : "false") << ","
        << "\"veto_applied\":"   << (e.veto_applied   ? "true" : "false") << ","
        << "\"provisional_rejected\":"
                                 << (e.provisional_rejected ? "true" : "false") << ","
        << "\"rep_rate_hz\":"    << e.rep_rate_hz                 << ","
        << "\"rep_cv\":"         << e.rep_cv                      << ","
        << "\"rep_n_onsets\":"   << e.rep_n_onsets                << ","
        // Derived, so an offline reader never has to re-derive the same
        // rule from the two booleans and get it subtly wrong. Mirrors
        // Recorder::classifyRejection exactly: the temporal guard is the
        // more specific cause because it only fires on an event the noise
        // rule had already decided to keep.
        << "\"reject_clause\":\""
        << (!e.gate_rejected ? "none" : (e.veto_applied ? "temporal" : "noise"))
        << "\"}";
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

bool EventPoller::snapshotNoiseFloor() {
    // Read the sample position FIRST. The clock can only advance, so
    // stamping before the copy makes the record's sample position a lower
    // bound on the audio the floor reflects — off by at most the copy's own
    // duration. Stamping after would make it an upper bound, which is the
    // wrong direction: a reader joining this against Stream A wants the
    // guarantee that the named sample had already been captured.
    const std::uint64_t at_sample = m_clock.now();

    // The tracker copies under its diagnostics lock; the 8 kB memcpy lands
    // on THIS thread, never on the audio thread. A tracker that models no
    // floor (or one polled before configure() sized it) returns false, and
    // we emit nothing rather than a record with an empty payload that a
    // reader would have to special-case.
    if (!m_pipeline.readNoiseFloor(m_floorScratch)) return false;
    if (m_floorScratch.empty()) return false;

    // Same float32-base64 encoding the sidecar uses for
    // noise_floor_at_first_event, down to the field names (n_bins,
    // encoding, data), so one decoder serves both artefacts. Reusing the
    // encoder rather than adding a second one is the whole reason this is
    // cheap to consume offline.
    std::ostringstream oss;
    oss << "{\"kind\":\"noise_floor\","
        << "\"at_sample\":"    << at_sample                     << ","
        << "\"n_bins\":"       << m_floorScratch.size()         << ","
        << "\"interval_sec\":" << m_cfg.noiseFloorInterval.count() << ","
        << "\"encoding\":\"float32_base64\","
        << "\"data\":\""
        << ::echobox::recorder::base64EncodeFloats(m_floorScratch)
        << "\"}";
    m_log.append(oss.str());
    m_floorsLogged.fetch_add(1, std::memory_order_release);
    return true;
}

void EventPoller::loop() {
    std::vector<EventFeatures> scratch;
    scratch.reserve(128);

    // Floor-snapshot cadence, counted in poll ticks rather than against a
    // wall clock: the poll interval is the only cadence this thread has, and
    // a tick counter cannot drift into a busy-catch-up burst the way a
    // deadline comparison can after the thread is descheduled. Zero disables
    // the trace entirely — the kill-switch discipline the whole overlay
    // follows, one level down.
    const std::uint64_t floorEveryNTicks =
        (m_cfg.noiseFloorInterval.count() > 0 && m_cfg.pollInterval.count() > 0)
            ? std::max<std::uint64_t>(
                  1,
                  static_cast<std::uint64_t>(m_cfg.noiseFloorInterval.count())
                      * 1000ULL
                      / static_cast<std::uint64_t>(m_cfg.pollInterval.count()))
            : 0;
    std::uint64_t ticks        = 0;
    bool          baselineDone = false;

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

        if (floorEveryNTicks > 0) {
            ++ticks;
            if (!baselineDone) {
                // Open the trace as soon as the detector HAS a floor, rather
                // than one full interval in. This poller is started before
                // DspPipeline::start(), so for the first few ticks there is
                // no tracker and then briefly an unseeded one; both answer
                // false. Retrying every tick costs one virtual call until it
                // lands (~1 tick in practice) and buys the session an
                // opening baseline, without which the night's drift has
                // nothing to be measured against.
                baselineDone = snapshotNoiseFloor();
                if (baselineDone) ticks = 0;
            } else if (ticks >= floorEveryNTicks) {
                ticks = 0;
                snapshotNoiseFloor();
            }
        }

        scratch.clear();
        // Destructive drain of the collection-only queue: each event is
        // handed to us exactly once, so no dedupe is needed and no event
        // can be lost to a race with the recorder's sidecar drain.
        if (!m_pipeline.drainCollectionEvents(scratch)) continue;

        for (const auto& e : scratch) {
            handleNew(e);
        }
    }
    m_log.flush();
}

} // namespace echobox::collection
