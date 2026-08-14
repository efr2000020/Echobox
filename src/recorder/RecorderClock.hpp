// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// The Recorder's time seam: an optional, injectable time source.
///
/// Why this lives in src/ at all
/// -----------------------------
/// The house rule for off-tree tooling is that duplication under
/// @c tools/ beats an abstraction leaking into @c src/ — see the header
/// comment in @c tools/replay/CMakeLists.txt. The clock is the one thing
/// that cannot follow that rule, because the problem is *inside*
/// @c Recorder::loop.
///
/// The recorder's state machine mixes two time bases:
///
///   - @c pollIntervalMs and @c silenceMs are **wall time**
///     (@c steady_clock::now + @c this_thread::sleep_for), while
///   - @c maxLengthMs is **audio time** (frames written to the WAV).
///
/// On the device those two are the same thing by construction: audio
/// arrives from ALSA at exactly 1 s per second. @c echobox-replay feeds
/// the same pipeline from disk at 20-30x real time, which decouples them
/// by a factor that varies with machine load. Measured consequences: the
/// same binary over the same 60 s input produced 635 / 650 / 650 clips on
/// three consecutive runs, and a 20 ms silence window became ~500 ms of
/// audio, so clip boundaries were set by @c maxLengthMs alone and replay
/// under-reported the device's own per-call recall by ~10 points.
///
/// No amount of code under @c tools/ can fix that. So the seam is here,
/// and it is kept as small as it can be: one pure-virtual interface, one
/// null pointer in @c RecorderConfig, and no implementation. The virtual
/// clock that drives replay lives off-tree in
/// @c tools/replay/VirtualClock.hpp, where it belongs.
///
/// What the field path pays
/// ------------------------
/// Nothing. The seam sits behind the @c ECHOBOX_RECORDER_CLOCK_INJECTION
/// compile-time gate, which @c src/recorder/CMakeLists.txt sets only when
/// the unit tests or the replay tool are being built. @c build_and_deploy.sh
/// builds neither, so the field binary is compiled from a translation unit
/// in which @c RecorderConfig has no @c clock member, @c Recorder::loop has
/// no dispatch branch, and @c nowSteady/@c nowWall collapse to the bare
/// @c steady_clock::now() / @c system_clock::now() calls the pre-seam
/// recorder made.
///
/// Verified rather than asserted, by disassembling @c Recorder.cpp.o from
/// a @c build_and_deploy.sh build before and after the seam landed:
/// identical symbol table, identical instruction count (6090), and one
/// two-instruction difference inside @c Recorder::loop where GCC picked
/// the mirrored encoding of the same test —
/// @c "cmp %rax,%rdx; jg" became @c "cmp %rdx,%rax; jl" for the
/// @c "idle >= silence" comparison. Same condition, same operand values,
/// opposite operand order; the choice is GCC's and is not reachable from
/// the source (rewriting the comparison as @c "silence <= idle" produces
/// a bit-identical object either way). Everything else in the field
/// recorder is instruction-for-instruction what it was.
///
/// This is the one place the project accepts an @c #ifdef in core code.
/// The usual rule (see @c tools/replay/CMakeLists.txt) is that a
/// build-time *directory* gate beats @c #ifdefs — but that rule only
/// works for code that can live in its own directory, and the two lines
/// that have to change are in the middle of @c Recorder's state machine.
/// A compile-time gate is what buys the byte-identity guarantee here;
/// leaving the seam always-compiled cost ~150 extra instructions in
/// @c Recorder::loop and made the guarantee unprovable.
///
/// When the gate IS set, the cost is still near zero: @c Recorder::loop
/// reads the pointer **once per run** and dispatches into a templated
/// loop body, so the poll itself has no branch and no indirection.

#include <chrono>

namespace echobox::recorder {

/**
 * @brief Injectable time source for the recorder's state machine.
 *
 * Implemented ONLY by off-tree harnesses (today: @c echobox::replay::
 * VirtualClock and the unit tests' scripted clock). A null
 * @c RecorderConfig::clock means "use the real clock", which is the only
 * value anything but those two ever supplies.
 *
 * @note This declaration is unconditional — an abstract class with no
 *       out-of-line members emits nothing into a TU that does not use it,
 *       so the header costs the field build nothing even though
 *       @c ECHOBOX_RECORDER_CLOCK_INJECTION gates every *use* of it.
 *
 * @note All three methods are called from the recorder thread (and, for
 *       @c now(), from whichever thread calls @c Recorder::stop()). They
 *       are never called from the audio thread, so an implementation may
 *       lock and block — @c VirtualClock's @c sleepFor does exactly that.
 */
class IRecorderClock {
public:
    virtual ~IRecorderClock() = default;

    /// Monotonic "now" driving the @c silenceMs idle timeout and the
    /// @c RECORDING_SAVED elapsed-time trace. Must never go backwards.
    virtual std::chrono::steady_clock::time_point now() const = 0;

    /// Wall "now" used to stamp recording filenames and the sidecar's
    /// @c capture_iso8601. Injected alongside @c now() so a replay run
    /// produces filenames derived from the audio it replayed rather than
    /// from when the run happened to be launched.
    virtual std::chrono::system_clock::time_point wallNow() const = 0;

    /// Advance to (at least) @c now() + @p d. An implementation is free to
    /// block on something other than real time — the virtual clock blocks
    /// until the harness has fed @p d worth of audio into the pipeline.
    virtual void sleepFor(std::chrono::milliseconds d) = 0;
};

} // namespace echobox::recorder
