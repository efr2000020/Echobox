// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Stream B: per-event WAV clips. For every detector event surfaced by
/// EventPoller (accepted AND gate-rejected) we extract a fixed-width
/// window around the event from a dedicated preroll buffer and dump it
/// to <collection-dir>/events/{accepted|rejected}/<window_start>.wav.
///
/// Filename convention: the 20-digit prefix is the absolute sample index
/// of the WAV's first sample (i.e. the event's start_sample minus
/// @c EventClipConfig::preSamples), NOT the event's trigger sample.
/// This lets an offline reader slice Stream A at exactly the number in
/// the filename and get byte-identical PCM back — the §3 A/B integrity
/// check depends on it. To correlate back to the event log, add
/// @c preSamples to the filename to recover the event's start_sample.
///
/// The buffer is a second @c recorder::PreRollBuffer instance sized for
/// the widest window we might need — the audio thread writes into it
/// alongside the recorder's own preroll buffer. Sample-position math is
/// carried through as a monotonic @c writeCount() offset from Session
/// start; conversion to filenames uses the sample-clock anchor.
///
/// See DATA_COLLECTION_IMPL_VALIDATION_PLAN §2.2 B.

#include "SampleClock.hpp"

#include "dsp/ISweepTracker.hpp"   // EventFeatures
#include "recorder/PreRollBuffer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace echobox::collection {

struct EventClipConfig {
    std::filesystem::path outputDir{"./collection/events"};
    int                   sampleRate{384000};
    int                   channels{1};
    std::size_t           hopSize{512};
    /// Samples to save before the event's start_sample. 100 ms at 384 kHz
    /// = 38 400 samples — comfortably wider than the recorder's own 50 ms
    /// preroll so a §3 A/B byte-identity check has generous overlap.
    std::uint32_t         preSamples{static_cast<std::uint32_t>(0.1 * 384000)};
    /// Samples to save after the event's end_sample.
    std::uint32_t         postSamples{static_cast<std::uint32_t>(0.1 * 384000)};
    /// Maximum time to wait for the audio thread to produce the post-
    /// event tail before giving up and writing a short clip. Guards
    /// against a shutdown mid-event stranding the writer thread forever.
    std::chrono::milliseconds tailWaitTimeout{500};
};

class EventClipWriter {
public:
    EventClipWriter(EventClipConfig cfg,
                    const recorder::PreRollBuffer& preRoll,
                    const SampleClock& clock);
    ~EventClipWriter();

    EventClipWriter(const EventClipWriter&)            = delete;
    EventClipWriter& operator=(const EventClipWriter&) = delete;

    void start();
    void stop();

    /**
     * @brief Enqueue an event for later WAV extraction.
     *
     * Called from @c EventPoller. Wait-free enough for the poll cadence
     * (100 ms), safe to call concurrently.
     */
    void enqueue(const EventFeatures& e, std::uint64_t start_sample,
                 std::uint64_t end_sample);

    std::uint64_t clipsWritten() const {
        return m_clipsWritten.load(std::memory_order_acquire);
    }
    /// Clips whose write was skipped — window aged out of the preroll,
    /// audio stream ended before postSamples were available, or the
    /// libsndfile open failed. Surfaced on STREAM_B_STOP.
    std::uint64_t clipsDropped() const {
        return m_clipsDropped.load(std::memory_order_acquire);
    }

private:
    struct Job {
        std::uint64_t start_sample;
        std::uint64_t end_sample;
        bool          gate_rejected;
        float         lo_hz;
        float         hi_hz;
    };

    void loop();
    bool writeClipWav(const Job& j);

    EventClipConfig                 m_cfg;
    const recorder::PreRollBuffer&  m_preRoll;
    const SampleClock&              m_clock;

    std::mutex                      m_mutex;
    std::condition_variable         m_cv;
    std::deque<Job>                 m_queue;

    std::atomic<bool>               m_running{false};
    std::thread                     m_thread;
    std::atomic<std::uint64_t>      m_clipsWritten{0};
    std::atomic<std::uint64_t>      m_clipsDropped{0};
};

} // namespace echobox::collection
