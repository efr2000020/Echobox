// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// EventClipWriter implementation. See EventClipWriter.hpp for the
/// contract.

#include "EventClipWriter.hpp"

#include "logging/Logger.hpp"

#include <array>
#include <cstdio>
#include <sndfile.h>
#include <system_error>

namespace echobox::collection {

namespace {

constexpr std::size_t kDrainChunkSamples = 4096;

std::string sampleFilename(std::uint64_t start_sample) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%020llu.wav",
                  static_cast<unsigned long long>(start_sample));
    return buf;
}

} // namespace

EventClipWriter::EventClipWriter(EventClipConfig cfg,
                                 const recorder::PreRollBuffer& preRoll,
                                 const SampleClock& clock)
    : m_cfg(std::move(cfg)), m_preRoll(preRoll), m_clock(clock) {}

EventClipWriter::~EventClipWriter() { stop(); }

void EventClipWriter::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;
    std::error_code ec;
    std::filesystem::create_directories(m_cfg.outputDir / "accepted", ec);
    std::filesystem::create_directories(m_cfg.outputDir / "rejected", ec);
    m_thread = std::thread(&EventClipWriter::loop, this);
}

void EventClipWriter::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    LS_INFO("collection",
            "STREAM_B_STOP clips_written=%llu clips_dropped=%llu",
            static_cast<unsigned long long>(m_clipsWritten.load()),
            static_cast<unsigned long long>(m_clipsDropped.load()));
}

void EventClipWriter::enqueue(const EventFeatures& e,
                              std::uint64_t start_sample,
                              std::uint64_t end_sample) {
    Job j{};
    j.start_sample  = start_sample;
    j.end_sample    = end_sample;
    j.gate_rejected = e.gate_rejected;
    j.lo_hz         = e.lo_hz;
    j.hi_hz         = e.hi_hz;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.push_back(j);
    }
    m_cv.notify_one();
}

void EventClipWriter::loop() {
    // Loop until the queue is drained even after stop() has been called —
    // dropping in-flight jobs at shutdown was a silent 2-3% event loss on
    // long field runs. The tail-wait inside the loop respects
    // tailWaitTimeout, which caps the shutdown lag per stalled job.
    for (;;) {
        Job j{};
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [&] {
                return !m_running.load(std::memory_order_acquire)
                    || !m_queue.empty();
            });
            if (m_queue.empty()) return;   // stopped AND drained
            j = m_queue.front();
            m_queue.pop_front();
        }

        // Wait for the audio thread to have captured up to
        // end_sample + postSamples, or give up after tailWaitTimeout and
        // let writeClipWav decide whether the window is usable. Do NOT
        // exit early on m_running=false — we still want to flush queued
        // jobs at shutdown.
        const std::uint64_t desiredEnd = j.end_sample + m_cfg.postSamples;
        const auto waitStart = std::chrono::steady_clock::now();
        while (m_clock.now() < desiredEnd) {
            if (std::chrono::steady_clock::now() - waitStart
                >= m_cfg.tailWaitTimeout) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!writeClipWav(j)) {
            m_clipsDropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool EventClipWriter::writeClipWav(const Job& j) {
    // Window: [start_sample - preSamples, end_sample + postSamples].
    // The filename encodes @c windowStart (not @c j.start_sample) so a
    // reader can slice Stream A at the number in the filename and get
    // byte-identical PCM back. The offline §3 A/B check depends on this.
    const std::uint64_t windowStart =
        (j.start_sample > m_cfg.preSamples) ? (j.start_sample - m_cfg.preSamples) : 0;
    const std::uint64_t windowEnd = j.end_sample + m_cfg.postSamples;
    if (windowEnd <= windowStart) return false;

    // The preroll buffer stores samples relative to its own writeCount().
    // The audio thread writes into it monotonically; its writeCount ==
    // total samples ever pushed. Convert absolute window → cursor:
    //   cursor.pos = windowStart (in absolute sample space).
    // The SampleClock and PreRollBuffer.writeCount() both start at 0 at
    // Application-run start and advance by the same batches in the same
    // captureLoop iteration, so they are identical.
    const std::uint64_t writeCount = m_preRoll.writeCount();
    if (windowEnd > writeCount) {
        // We waited above; the audio thread simply didn't produce more.
        // Skip this event rather than write a WAV that ends abruptly on
        // a stale cursor — the §3 A/B check would fail on a partial clip.
        LS_WARN("collection",
                "stream-b: event %llu still incomplete at clip time "
                "(want %llu, have %llu) — skipping",
                static_cast<unsigned long long>(j.start_sample),
                static_cast<unsigned long long>(windowEnd),
                static_cast<unsigned long long>(writeCount));
        return false;
    }
    if (writeCount - windowStart > m_preRoll.capacity()) {
        // The window has aged out of the preroll ring; the buffer isn't
        // sized for how long we sat on this job. Not a spec failure —
        // just tell the operator and skip.
        LS_WARN("collection",
                "stream-b: event %llu aged out of preroll (need %llu behind, "
                "capacity %zu) — skipping",
                static_cast<unsigned long long>(j.start_sample),
                static_cast<unsigned long long>(writeCount - windowStart),
                m_preRoll.capacity());
        return false;
    }

    recorder::PreRollBuffer::Cursor cursor;
    cursor.pos   = windowStart;
    cursor.valid = true;

    const auto path = m_cfg.outputDir
        / (j.gate_rejected ? "rejected" : "accepted")
        / sampleFilename(windowStart);

    SF_INFO info{};
    info.samplerate = m_cfg.sampleRate;
    info.channels   = m_cfg.channels;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* sf = sf_open(path.string().c_str(), SFM_WRITE, &info);
    if (!sf) {
        LS_ERROR("collection", "stream-b: sf_open %s failed: %s",
                 path.string().c_str(), sf_strerror(nullptr));
        return false;
    }

    std::array<std::int16_t, kDrainChunkSamples> buf{};
    std::uint64_t written = 0;
    const std::uint64_t total = windowEnd - windowStart;
    while (written < total) {
        std::size_t lost = 0;
        const std::size_t got = m_preRoll.read(cursor,
            std::span<std::int16_t>(buf.data(),
                std::min(buf.size(),
                         static_cast<std::size_t>(total - written))),
            lost);
        if (lost > 0) {
            LS_WARN("collection", "stream-b: preroll lost %zu samples for event %llu",
                    lost, static_cast<unsigned long long>(j.start_sample));
        }
        if (got == 0) break;
        sf_write_short(sf, buf.data(), static_cast<sf_count_t>(got));
        written += got;
    }
    sf_close(sf);
    m_clipsWritten.fetch_add(1, std::memory_order_release);
    return true;
}

} // namespace echobox::collection
