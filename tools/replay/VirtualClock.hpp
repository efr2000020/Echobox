// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Sample-driven virtual clock + feeder/recorder barrier for echobox-replay.
///
/// The problem
/// -----------
/// The recorder's state machine mixes wall time (@c pollIntervalMs,
/// @c silenceMs) with audio time (@c maxLengthMs, counted in frames
/// written). On the device those are the same thing; in replay, which
/// feeds from disk at 20-30x real time, they are not. Measured: 635 /
/// 650 / 650 clips from three consecutive runs of the same binary over
/// the same 60 s file, and a 20 ms silence window stretched to ~500 ms of
/// audio, so clips were closed by @c maxLengthMs alone and the whole
/// silence branch was dead code.
///
/// The fix
/// -------
/// Two halves, both here:
///
/// 1. **Time is audio.** The clock only moves when the driver says so,
///    and the driver moves it by exactly the amount of audio it just fed
///    (see @c LockstepFeeder in main.cpp). One millisecond of recorder
///    time is one millisecond of audio, at any host speed. @c wallNow()
///    is the same quantity offset from a fixed epoch, so filenames and
///    @c capture_iso8601 describe the audio rather than when the run
///    happened to be launched.
///
/// 2. **The feeder and the recorder take turns.** A virtual clock alone
///    is not enough: the recorder thread would still race the feeder for
///    who observes which detector snapshot. @c sleepFor() parks the
///    recorder until the feeder has fed the corresponding audio, and
///    @c waitUntilRecorderIdle() parks the feeder until the recorder has
///    finished the poll it was woken for. The two threads therefore
///    strictly alternate, and the recorder observes exactly the sequence
///    of snapshots a real-time device would.
///
/// The result is deterministic by construction — not "usually stable" —
/// while still running far faster than real time, because the pace is set
/// by how fast the host can do the FFTs, not by a sleep.
///
/// @note Ordinary blocking synchronisation (mutex + condition_variable) is
///       correct here and is *not* a violation of the project's
///       no-locks-on-the-audio-thread rule: replay has no audio thread. The
///       feeder is an ordinary worker reading a file, and it is supposed to
///       block. Nothing in this file is compiled into the shipping binary.

#include "recorder/RecorderClock.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace echobox::replay {

/**
 * @brief Virtual time source + turn-taking barrier for a replay run.
 *
 * Thread model: exactly one "recorder" thread (the one inside
 * @c echobox::recorder::Recorder) calls the @c IRecorderClock methods, and
 * exactly one "feeder" thread (replay's main thread) calls the driver-side
 * methods. @c now() may additionally be called by whichever thread runs
 * @c Recorder::stop(), which is the feeder thread — safe, because by then
 * the recorder thread has been joined.
 */
class VirtualClock final : public echobox::recorder::IRecorderClock {
public:
    /**
     * @param wallEpoch Wall instant that virtual time zero maps to. Only
     *                  affects recording filenames and the sidecars'
     *                  @c capture_iso8601 / @c boot_iso8601 — it must be a
     *                  fixed value, never @c system_clock::now(), or replay
     *                  output stops being reproducible.
     */
    explicit VirtualClock(std::chrono::system_clock::time_point wallEpoch);

    // --- IRecorderClock: called on the recorder thread ---

    std::chrono::steady_clock::time_point now() const override;
    std::chrono::system_clock::time_point wallNow() const override;

    /// Park until virtual time has advanced by @p d. Also publishes "the
    /// recorder is idle and waiting for time @c now+d" to the feeder, which
    /// is what makes the alternation work. After @c release() this degrades
    /// to a real sleep so a shutting-down recorder cannot deadlock.
    void sleepFor(std::chrono::milliseconds d) override;

    // --- driver side: called on the feeder thread ---

    /**
     * @brief Block until the recorder has finished the poll it was last
     *        woken for and is parked waiting for a *future* virtual time.
     *
     * The "future" part is what makes this race-free. Testing a bare
     * "is it parked?" flag would let the feeder run ahead in the window
     * between @c advanceTo() notifying and the recorder actually waking:
     * the flag is still set from the previous park. Requiring
     * @c m_wakeNs > @c m_ns instead means the condition can only be true
     * once the recorder has re-parked with a fresh target, i.e. once it
     * has actually consumed the time we just published.
     *
     * A recorder that skips a tick (possible when the sample rate makes a
     * poll interval a non-integer number of frames) is handled by the same
     * test: it is already parked for a future time, so the feeder proceeds.
     */
    void waitUntilRecorderIdle();

    /**
     * @brief Move virtual now to @p virtualNow and wake the recorder if
     *        that reaches its target.
     *
     * Absolute, not incremental: the driver passes a value computed from
     * an exact tick index, so no amount of integer rounding can make the
     * clock drift away from the audio over a ten-hour corpus. Values that
     * would move time backwards are ignored — the recorder's silence
     * timeout would silently misbehave rather than fail loudly.
     */
    void advanceTo(std::chrono::nanoseconds virtualNow);

    /// Permanently unblock both sides. Call once, immediately before
    /// @c Recorder::stop(), otherwise the recorder thread parks forever
    /// waiting for audio that will never be fed and the join never returns.
    void release();

    /// Virtual nanoseconds since epoch. Diagnostics only.
    std::uint64_t nowNs() const;

private:
    const std::chrono::system_clock::time_point m_wallEpoch;

    mutable std::mutex      m_mutex;
    std::condition_variable m_cvRecorder;  ///< Signalled by advanceTo/release.
    std::condition_variable m_cvFeeder;    ///< Signalled when the recorder parks.

    std::uint64_t m_ns{0};        ///< Virtual now.
    std::uint64_t m_wakeNs{0};    ///< Virtual time the recorder is waiting for.
    bool          m_parked{false};///< Recorder is inside sleepFor().
    bool          m_released{false};
};

} // namespace echobox::replay
