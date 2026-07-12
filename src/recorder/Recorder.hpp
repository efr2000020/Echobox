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
#include <chrono>
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
    /// Wall-clock instant the device booted. Stamped into every sidecar
    /// alongside the per-recording capture timestamp; lets the offline
    /// tuning tools reason about uptime and event-rate windows. Defaults
    /// to "now" if the caller doesn't override it.
    std::chrono::system_clock::time_point bootWall{std::chrono::system_clock::now()};
    /// Emit a JSON sidecar next to each saved WAV. Always-on by default —
    /// the per-file cost is ~10 KB of disk; the analytical value at the
    /// next tuning session is large. Set to false to suppress (e.g. tests
    /// that exercise only the recording path).
    bool                  writeSidecar{true};
    /// Discard clips whose window saw no bat-like event (i.e. every
    /// detector event during the clip was gate-rejected). Together with
    /// the detector's @c sweep_gate_enabled tunable, this is the recorder
    /// half of the cricket filter — a pure-cricket recording is written
    /// to a temp WAV, then aborted at close instead of finalised. Wired
    /// to the @c --cricket-filter CLI flag; disabling both halves with
    /// that one switch restores the behaviour of the pre-gate recorder.
    bool                  cricketDiscard{true};
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
             IDetectorStateProvider& detector);
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
    IDetectorStateProvider&       m_detector;

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
    // Snapshot of the detector's kept-events counter at beginRecording.
    // A clip whose counter hasn't advanced at endRecording contained no
    // bat-like event → discard it if @c cricketDiscard is set.
    std::uint64_t                         m_batLikeAtStart{0};
    // Set by @c appendLiveAudio when the max-length cap forces a close
    // while an event is still active — the triggering event has not yet
    // stamped the counter, so @c endRecording must NOT cricket-discard
    // this clip (the discard decision only applies to closed events).
    bool                                  m_lastCloseWasMaxLenActive{false};
};

} // namespace echobox::recorder
