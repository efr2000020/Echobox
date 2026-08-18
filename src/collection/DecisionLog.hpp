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
///                                   R2v3 firmware's actual save/discard
///                                   verdict for the clip that spans
///                                   these events.
///
/// The offline verifier (§4.1 tool) correlates event↔decision by sample
/// overlap and diffs against @c recorder_model.py; matches promote the
/// model from YELLOW to GREEN.

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
