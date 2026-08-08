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
#include <deque>
#include <filesystem>
#include <memory>
#include <random>
#include <thread>

namespace echobox::recorder {

class WavWriter;

/// --save-rejected mode selector. See RecorderConfig::saveRejected.
enum class SaveRejectedMode {
    Off,       ///< No rejected/ dir; feature entirely disabled.
    All,       ///< Save every rejected clip.
    Sample,    ///< Save a random 1-in-N.
    Boundary,  ///< Save only near-threshold near-misses.
};

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
    /// Mirrors the shipping default in @c echobox::app::Config so a
    /// RecorderConfig built ex-nihilo (e.g. from a test fixture) matches
    /// the production geometry. The two must be updated together.
    std::uint32_t         preRollMs{10};
    /// Quiet time the detector must show before the recording closes.
    /// Mirrors the shipping default in @c echobox::app::Config. Must sit
    /// at or above the cricket-filter counter-race floor derived in
    /// @c ConfigValidator::checkSilenceExceedsHangover (16 ms at the
    /// shipping 384 kHz / hop-512 defaults).
    std::uint32_t         silenceMs{20};
    /// Minimum end-to-end WAV length. Files shorter than this are deleted
    /// instead of finalized. @c 0 disables the gate.
    std::uint32_t         minLengthMs{0};
    /// Maximum end-to-end WAV length. Recordings reaching this length close
    /// early. @c 0 disables the cap. Mirrors the shipping default in
    /// @c echobox::app::Config.
    std::uint32_t         maxLengthMs{40};
    /// How often the recorder polls the detector state, in ms.
    /// 1 ms because (i) it is a term in the cricket-filter silence floor
    /// (@c ConfigValidator::checkSilenceExceedsHangover), and at the
    /// ~40 ms end-to-end clip budget the short-clip release round pushed
    /// for, every millisecond of that floor counts; (ii) the detector's
    /// active window can be as short as ~5 ms when the provisional gate
    /// suppresses, so a 5 ms poll could miss the leading edge entirely.
    std::uint32_t         pollIntervalMs{1};
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

    // --- Rejected-capture observability ---
    // When @c saveRejected is not @c Off, a clip that would be cricket-
    // discarded is instead renamed into @c outputDir/rejected/ with a
    // sidecar (rejection reason + the features the detector already
    // computed). Off (default) keeps the recorder byte-identical to
    // pre-feature behaviour — no rejected/ dir is ever created and the
    // discard branch continues to abort the WAV.
    SaveRejectedMode      saveRejected{SaveRejectedMode::Off};
    /// 1-in-N sampling ratio for @c SaveRejectedMode::Sample. Must be >= 1.
    std::uint32_t         saveRejectedSampleN{500};
    /// Storage governor: at most N rejected clips per rolling hour.
    /// @c 0 disables the cap (unlimited — for local offline-validation runs).
    std::uint32_t         saveRejectedMaxPerHour{200};
    /// Skip the rejected-sink write when free disk drops below this many MB.
    /// @c 0 disables the check. Bounds any risk of the observability sink
    /// filling the SD card faster than the operator can rotate it.
    std::uint32_t         saveRejectedMinDiskMb{100};
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

    // --- Rejected-capture sink helpers ---
    //
    // saveRejectedClip() is called from endRecording() ONLY on the
    // cricket-discard branch, before the WAV is aborted, when the
    // sink mode + governor allow. It renames the temp WAV into
    // outputDir/rejected/ and writes a sidecar next to it. Returns
    // true iff the clip was preserved (caller then skips abort).
    bool shouldSaveRejected(const class SidecarPayload& payload);
    bool saveRejectedClip(const class SidecarPayload& payload);
    bool rejectedGovernorAllows();
    void recordRejectedWrite();

    RecorderConfig                m_cfg;
    const PreRollBuffer&          m_preRoll;
    IDetectorStateProvider&       m_detector;

    FilenameBuilder               m_names;
    std::atomic<bool>             m_running{false};
    std::thread                   m_thread;

    // Rolling per-hour timestamp ring for the storage governor. The oldest
    // entry is popped before comparing against the cap; drift-free and
    // proportional in size to the actual write rate. Recorder-thread-only —
    // no lock needed.
    std::deque<std::chrono::steady_clock::time_point> m_rejectedWriteTimes;
    /// Per-process rejected-write count since boot. Logged for tuning.
    std::uint64_t                 m_rejectedWrittenTotal{0};
    /// PRNG for @c SaveRejectedMode::Sample; recorder-thread-only.
    std::mt19937                  m_rng{std::random_device{}()};
    /// Sticky low-disk state: once we log LOW_DISK we suppress subsequent
    /// rejected-writes silently until the check clears again.
    bool                          m_rejectedLowDiskLogged{false};

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
