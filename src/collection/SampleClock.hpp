// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// The one primitive every collection artefact depends on: a monotonic
/// uint64 sample counter from capture start, owned by the audio front-end.
///
/// See DATA_COLLECTION_IMPL_VALIDATION_PLAN §2.1. Every filename, every
/// JSONL record, and every WAV chunk in every stream stamps its start-sample
/// from this counter so all four streams are alignable at the sample level
/// without ever consulting wall-clock time.

#include <atomic>
#include <cstdint>

namespace echobox::collection {

/**
 * @brief Monotonic sample counter, stamped by the audio capture loop.
 *
 * Advanced exactly once per ALSA read by the number of frames delivered.
 * Read from any thread via @c now(); the getter is a single acquire load
 * and is safe from the recorder / DSP / governor threads. The audio thread
 * owns the writer role.
 *
 * At 384 kHz mono this counter takes ~1.5 million years to wrap uint64,
 * so no rollover handling is needed.
 *
 * @note Kill-switch discipline: @c advance() is called from
 *       @c Application::captureLoop only when
 *       @c CollectionConfig::enabled is true, so a shipping unit with the
 *       overlay disabled never executes the atomic RMW.
 *
 * Relationship to @c recorder::IRecorderClock
 * ------------------------------------------
 * The two do not overlap and cannot double-count, because they measure
 * different quantities:
 *
 *  - This clock counts **samples captured**. It is advanced once per
 *    ALSA read, in the same @c captureLoop statement group that pushes
 *    the same batch into @c Recorder's @c PreRollBuffer — so
 *    @c SampleClock::now() and @c PreRollBuffer::writeCount() are equal
 *    by construction, and every collection artefact can be sliced out of
 *    either without conversion.
 *  - @c IRecorderClock supplies **steady and wall time** to the
 *    recorder's state machine (the @c silenceMs idle timeout, filename
 *    stamps). It exists so @c echobox-replay can run the pipeline faster
 *    than real time; it is compiled out of the field build entirely.
 *
 * Nothing reads one to derive the other. In particular
 * @c Recorder::m_clipStartSample — the only sample-space value the
 * recorder publishes to this overlay — is taken from the pre-roll
 * cursor, i.e. from the audio thread's write count, never from a clock.
 * So a clip's reported sample range stays correct even under an injected
 * time source running at 20-30x real time.
 */
class SampleClock {
public:
    SampleClock() = default;

    SampleClock(const SampleClock&)            = delete;
    SampleClock& operator=(const SampleClock&) = delete;

    /// Audio thread: publish @p framesJustCaptured as newly-consumed samples.
    /// Release semantics: any subsequent load-acquire in another thread
    /// sees the samples that produced this value.
    void advance(std::uint64_t framesJustCaptured) {
        m_pos.fetch_add(framesJustCaptured, std::memory_order_release);
    }

    /// Any thread: current sample position (frames captured so far).
    std::uint64_t now() const {
        return m_pos.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::uint64_t> m_pos{0};
};

} // namespace echobox::collection
