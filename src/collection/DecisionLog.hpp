// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Stream C: append-only JSONL decision log. See DATA_COLLECTION_IMPL_
/// VALIDATION_PLAN §2.2 C. Records fall into two families that offline
/// tools correlate by sample range:
///
///   { "kind": "event",    ... }  — emitted by @c EventPoller from the
///                                   DSP tracker's pending-events queue.
///                                   Every detector event (accepted AND
///                                   gate-rejected) gets one record.
///   { "kind": "decision", ... }  — emitted by @c Recorder via
///                                   @c IRecorderDecisionSink at
///                                   clip-close. Carries the shipping
///                                   firmware's actual save/discard
///                                   verdict for the clip that spans
///                                   these events, plus the gate clause
///                                   that attributed any rejection.
///   { "kind":"noise_floor", ...} — emitted by @c EventPoller on a timer
///                                   (default 60 s). The detector's live
///                                   per-bin floor, float32-base64 in the
///                                   same encoding and field names the WAV
///                                   sidecar uses, stamped with the
///                                   sample-clock position it was taken at.
///                                   Unlike the other two this is not
///                                   event-driven: it is the only record
///                                   that keeps arriving through a stretch
///                                   with no detections, which is exactly
///                                   the stretch in which a chorus masking
///                                   the 20–45 kHz band would otherwise
///                                   leave no trace.
///
/// Offline tools correlate the event/decision families by sample overlap: a
/// decision covers the events whose @c start_sample falls in
/// [@c clip_start_sample, @c clip_end_sample]. Floor records join to both by
/// @c at_sample, and to Stream A by the same number.
///
/// Readers MUST ignore record kinds they do not recognise rather than
/// failing on them — that is what let this third kind be added without
/// touching the offline verifiers.

#include <filesystem>
#include <mutex>
#include <string>

namespace echobox::collection {

/**
 * @brief Thread-safe append-only JSONL writer.
 *
 * Multiple threads (EventPoller, Recorder decision sink) call @c append()
 * concurrently; the writer serialises them on an internal mutex. The
 * file is opened once at construction and left open — atomic writes are
 * not needed for an append-only log where a partial trailing line is
 * treated as truncated by every reader we ship.
 */
class DecisionLog {
public:
    /// @param path Full path to the JSONL. Parent dirs are created on
    ///             first open. Overwrites the file if it already exists,
    ///             so a fresh session starts with an empty log (the plan
    ///             requires immutable per-session artefacts).
    explicit DecisionLog(std::filesystem::path path);
    ~DecisionLog();

    DecisionLog(const DecisionLog&)            = delete;
    DecisionLog& operator=(const DecisionLog&) = delete;

    /// Append one JSONL record. @p line must NOT contain an embedded
    /// newline; a single trailing '\n' is added by the writer.
    bool append(const std::string& line);

    /// Flush the underlying ofstream. Called by the Session on stop().
    void flush();

    std::filesystem::path path() const { return m_path; }

private:
    std::filesystem::path m_path;
    std::mutex            m_mutex;
    // Kept as a plain FILE* so append() is one fwrite + one fflush; the
    // JSONL contract is line-atomic within a single process and this is
    // the simplest way to get that on Linux.
    void*                 m_fp{nullptr};
};

} // namespace echobox::collection
