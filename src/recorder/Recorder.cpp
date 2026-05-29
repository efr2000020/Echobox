#include "Recorder.hpp"
#include "WavWriter.hpp"
#include "logging/Logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

namespace litespec::recorder {

namespace {
constexpr std::size_t kDrainChunkSamples = 4096;
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

    LS_INFO("recorder", "started preroll=%ums silence=%ums dir=%s",
            m_cfg.preRollMs, m_cfg.silenceMs, m_cfg.outputDir.string().c_str());

    while (m_running.load(std::memory_order_acquire)) {
        const auto s = m_detector.snapshot();

        if (m_state == State::Idle) {
            if (s.active) {
                beginRecording(s);
            }
        } else {
            appendLiveAudio();
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

    LS_INFO("recorder", "open file=%s preroll=%zu samples",
            m_currentTempPath.filename().string().c_str(), preRollSamples);

    m_state = State::Active;
    appendLiveAudio();
}

void Recorder::appendLiveAudio() {
    if (!m_writer) return;
    std::array<std::int16_t, kDrainChunkSamples> chunk{};

    for (;;) {
        std::size_t lost = 0;
        const std::size_t got = m_preRoll.read(m_cursor,
                                               std::span<std::int16_t>(chunk),
                                               lost);
        if (lost > 0) {
            LS_WARN("recorder", "preroll overrun: dropped %zu samples", lost);
        }
        if (got == 0) break;
        m_writer->write(std::span<const std::int16_t>(chunk.data(), got));
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

    const auto finalPath = m_names.finalPath(m_eventStartWall, durationMs, loHz, hiHz);
    m_writer->closeAndRename(finalPath);
    m_writer.reset();

    LS_INFO("recorder", "close file=%s frames=%llu duration=%ums band=%.1f-%.1fkHz",
            finalPath.filename().string().c_str(),
            static_cast<unsigned long long>(frames), durationMs,
            loHz / 1000.0f, hiHz / 1000.0f);

    m_state = State::Idle;
}

} // namespace litespec::recorder
