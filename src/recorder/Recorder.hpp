// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Recorder gate: polls the detector and writes one WAV per event, with
/// configurable pre-roll, hangover, and min/max length.

#include "DetectorStateProvider.hpp"
#include "FilenameBuilder.hpp"
#include "PreRollBuffer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>

namespace echobox::recorder {

class WavWriter;

/**
 * @brief Construction-time configuration for Recorder.
 *
 * All length fields are end-to-end: a saved WAV spans pre-roll + the active
 * region + the trailing silence (up to @c silenceMs of it).
 */
struct RecorderConfig {
    std::filesystem::path outputDir{"./recordings"};
    int                   sampleRate{384000};
    int                   channels{1};
    /// Audio to keep from *before* the leading edge of each event.
    std::uint32_t         preRollMs{1000};
    /// Quiet time the detector must show before the recording closes.
    std::uint32_t         silenceMs{100};
    /// Minimum end-to-end WAV length. Files shorter than this are deleted
    /// instead of finalized. @c 0 disables the gate.
    std::uint32_t         minLengthMs{0};
    /// Maximum end-to-end WAV length. Recordings reaching this length close
    /// early. @c 0 disables the cap.
    std::uint32_t         maxLengthMs{50};
    /// How often the recorder polls the detector state, in ms.
    std::uint32_t         pollIntervalMs{5};
};

/**
 * @brief Detection-driven WAV writer.
 *
 * Subscribes to an @c IDetectorStateProvider and a @c PreRollBuffer fed
 * continuously by the audio thread. On a detection's rising edge: opens a
 * temp WAV, dumps the configured pre-roll, then streams live samples until
 * the detector has been quiet for @c silenceMs. On close, renames the file
 * to its canonical name (timestamp + duration + firing-band span).
 *
 * @note Owns one worker thread. Non-copyable. @c PreRollBuffer and
 *       @c IDetectorStateProvider lifetimes must outlive this object.
 */
class Recorder {
public:
    /**
     * @param cfg      Recorder configuration (copied).
     * @param preRoll  Sample reservoir for the lead-in dump. Lifetime must
     *                 outlive the recorder.
     * @param detector Detector-state source polled by the worker. Lifetime
     *                 must outlive the recorder.
     */
    Recorder(RecorderConfig cfg,
             const PreRollBuffer& preRoll,
             const IDetectorStateProvider& detector);
    ~Recorder();

    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    /// Spawn the worker thread. Idempotent.
    void start();
    /// Stop the worker thread and finalize any in-progress recording.
    /// Idempotent. Safe to call from any thread.
    void stop();

private:
    enum class State { Idle, Active };

    void loop();

    void beginRecording(const DetectorStateSnapshot& s);
    void appendLiveAudio();
    void endRecording();

    RecorderConfig                m_cfg;
    const PreRollBuffer&          m_preRoll;
    const IDetectorStateProvider& m_detector;

    FilenameBuilder               m_names;
    std::atomic<bool>             m_running{false};
    std::thread                   m_thread;

    // --- active recording state ---
    State                                m_state{State::Idle};
    std::unique_ptr<WavWriter>           m_writer;
    PreRollBuffer::Cursor                m_cursor;
    std::chrono::steady_clock::time_point m_eventStartSteady;
    std::chrono::system_clock::time_point m_eventStartWall;
    std::chrono::steady_clock::time_point m_lastActiveTime;
    float                                 m_eventLoHz{0.0f};
    float                                 m_eventHiHz{0.0f};
    std::filesystem::path                 m_currentTempPath;
};

} // namespace echobox::recorder
