#include "Recorder.hpp"
#include "WavWriter.hpp"
#include "logging/Logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

namespace echobox::recorder {

namespace {
constexpr std::size_t kDrainChunkSamples = 4096;

std::uint64_t framesForMs(std::uint32_t ms, int sampleRate) {
    return static_cast<std::uint64_t>(ms) * static_cast<std::uint64_t>(sampleRate) / 1000ULL;
}
} // namespace

Recorder::Recorder(RecorderConfig cfg,
                   const PreRollBuffer& preRoll,
                   const IDetectorStateProvider& detector)
    : m_cfg(std::move(cfg)),
      m_preRoll(preRoll),
      m_detector(detector),
      m_names(m_cfg.outputDir) {}

Recorder::~Recorder() {
    stop();
}

void Recorder::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;
    m_thread = std::thread(&Recorder::loop, this);
}

void Recorder::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    if (m_thread.joinable()) m_thread.join();
    if (m_state == State::Active) {
        endRecording();
    }
}

void Recorder::loop() {
    const auto pollInterval = std::chrono::milliseconds(m_cfg.pollIntervalMs);
    const auto silence      = std::chrono::milliseconds(m_cfg.silenceMs);

    LS_INFO("recorder", "started preroll=%ums silence=%ums min=%ums max=%ums dir=%s",
            m_cfg.preRollMs, m_cfg.silenceMs,
            m_cfg.minLengthMs, m_cfg.maxLengthMs,
            m_cfg.outputDir.string().c_str());

    while (m_running.load(std::memory_order_acquire)) {
        const auto s = m_detector.snapshot();

        if (m_state == State::Idle) {
            if (s.active) {
                beginRecording(s);
            }
        } else {
            appendLiveAudio();
            // appendLiveAudio() flips m_state back to Idle if the max-length
            // cap closed the file — re-check before processing the detector
            // snapshot so we don't double-end an already-closed recording.
            if (m_state == State::Idle) {
                continue;
            }
            if (s.active) {
                m_lastActiveTime = std::chrono::steady_clock::now();
                if (s.loHz > 0.0f) m_eventLoHz = std::min(m_eventLoHz, s.loHz);
                if (s.hiHz > 0.0f) m_eventHiHz = std::max(m_eventHiHz, s.hiHz);
            } else {
                const auto idle = std::chrono::steady_clock::now() - m_lastActiveTime;
                if (idle >= silence) {
                    endRecording();
                }
            }
        }

        std::this_thread::sleep_for(pollInterval);
    }

    // On shutdown, flush any in-progress recording so we don't lose it.
    if (m_state == State::Active) {
        appendLiveAudio();
        endRecording();
    }
    LS_INFO("recorder", "stopped");
}

void Recorder::beginRecording(const DetectorStateSnapshot& s) {
    m_eventStartWall   = std::chrono::system_clock::now();
    m_eventStartSteady = std::chrono::steady_clock::now();
    m_lastActiveTime   = m_eventStartSteady;
    m_eventLoHz        = s.loHz > 0.0f ? s.loHz : 0.0f;
    m_eventHiHz        = s.hiHz > 0.0f ? s.hiHz : 0.0f;

    m_currentTempPath = m_names.tempPath(m_eventStartWall);
    m_writer = std::make_unique<WavWriter>(m_currentTempPath,
                                           m_cfg.sampleRate, m_cfg.channels);

    const std::size_t preRollSamples =
        static_cast<std::size_t>(m_cfg.preRollMs) * m_cfg.sampleRate / 1000u;
    m_cursor = m_preRoll.openCursor(preRollSamples);

    // Distinctive prefix so an operator can `grep RECORDING_ logs/*` to walk
    // every saved file's lifecycle, and the filename is the obvious anchor.
    LS_INFO("recorder", "RECORDING_OPEN file=%s preroll=%zu samples band=%.1f-%.1fkHz",
            m_currentTempPath.filename().string().c_str(), preRollSamples,
            m_eventLoHz / 1000.0f, m_eventHiHz / 1000.0f);

    m_state = State::Active;
    appendLiveAudio();
}

void Recorder::appendLiveAudio() {
    if (!m_writer) return;
    std::array<std::int16_t, kDrainChunkSamples> chunk{};

    // 0 disables the cap. Pre-compute as a frame count so the per-chunk check
    // is one comparison, not a division.
    const std::uint64_t maxFrames = (m_cfg.maxLengthMs > 0)
        ? framesForMs(m_cfg.maxLengthMs, m_cfg.sampleRate)
        : std::numeric_limits<std::uint64_t>::max();

    for (;;) {
        if (m_writer->framesWritten() >= maxFrames) {
            endRecording();   // close + rename (or discard if under min)
            return;
        }
        std::size_t lost = 0;
        const std::size_t got = m_preRoll.read(m_cursor,
                                               std::span<std::int16_t>(chunk),
                                               lost);
        if (lost > 0) {
            LS_WARN("recorder", "preroll overrun: dropped %zu samples", lost);
        }
        if (got == 0) break;

        // Clip the write so we never overshoot the budget by up to a chunk.
        const std::uint64_t remaining = maxFrames - m_writer->framesWritten();
        const std::size_t   toWrite   = static_cast<std::size_t>(
            std::min<std::uint64_t>(got, remaining));
        m_writer->write(std::span<const std::int16_t>(chunk.data(), toWrite));
        if (toWrite < got) {
            endRecording();
            return;
        }
    }
}

void Recorder::endRecording() {
    if (!m_writer) {
        m_state = State::Idle;
        return;
    }

    const std::uint64_t frames = m_writer->framesWritten();
    const std::uint32_t durationMs = static_cast<std::uint32_t>(
        (frames * 1000ULL) / static_cast<std::uint64_t>(m_cfg.sampleRate));

    const float loHz = m_eventLoHz > 0.0f ? m_eventLoHz : 0.0f;
    const float hiHz = m_eventHiHz > 0.0f ? m_eventHiHz : 0.0f;

    // Min-length gate: anything shorter is dropped on the floor rather than
    // finalized, so the output dir stays free of clip-sized noise events.
    if (m_cfg.minLengthMs > 0
        && frames < framesForMs(m_cfg.minLengthMs, m_cfg.sampleRate)) {
        LS_INFO("recorder", "RECORDING_DISCARDED file=%s duration=%ums (< minLengthMs=%u) band=%.1f-%.1fkHz",
                m_currentTempPath.filename().string().c_str(),
                durationMs, m_cfg.minLengthMs,
                loHz / 1000.0f, hiHz / 1000.0f);
        m_writer->abort();
        m_writer.reset();
        m_state = State::Idle;
        return;
    }

    const auto finalPath = m_names.finalPath(m_eventStartWall, durationMs);
    m_writer->closeAndRename(finalPath);
    m_writer.reset();

    // Distinctive prefix + final filename + the steady-clock elapsed since
    // OPEN, so a false-positive WAV is easy to correlate with the [dsp.bed]
    // log lines stamped within that same window.
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_eventStartSteady).count();
    LS_INFO("recorder", "RECORDING_SAVED file=%s frames=%llu duration=%ums elapsed=%lldms band=%.1f-%.1fkHz",
            finalPath.filename().string().c_str(),
            static_cast<unsigned long long>(frames), durationMs,
            static_cast<long long>(elapsedMs),
            loHz / 1000.0f, hiHz / 1000.0f);

    m_state = State::Idle;
}

} // namespace echobox::recorder
