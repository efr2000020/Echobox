// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Stream A: continuous reference-audio writer. Drains ReferenceRing into
/// rolling WAV chunks named by start-sample + UTC and appends one line
/// per chunk to a chunk manifest. The detector/recorder never touches
/// this data — it is the ground truth Stream A/B byte-identity and
/// Stream A/C alignment checks compare against (§3 in the plan).

#include "ReferenceRing.hpp"
#include "SampleClock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace echobox::collection {

/**
 * @brief Chunk cadence and I/O layout.
 *
 * A ~60 s chunk at 384 kHz mono int16 is ~46 MiB — small enough to open,
 * fsync, and rotate cleanly on an SD card; large enough that manifest
 * bookkeeping is not on the hot path. Overridable so unit tests can
 * exercise the rotation logic with sub-second chunks.
 */
struct ContinuousWriterConfig {
    std::filesystem::path outputDir{"./collection/reference"};
    int                   sampleRate{384000};
    int                   channels{1};
    std::uint32_t         chunkDurationSec{60};

    /// Manifest filename inside outputDir. One JSONL record per closed
    /// chunk: file, start_sample, end_sample, wall_start_iso8601,
    /// wall_end_iso8601, drops_snapshot.
    std::string           manifestName{"chunks.jsonl"};
};

/**
 * @brief Continuous reference writer thread.
 *
 * Owns exactly one worker. @c start() spawns it; @c stop() signals and
 * joins, flushing the in-progress chunk. Idempotent on both sides.
 *
 * Sample-position bookkeeping: the writer maintains a local
 * @c samplesDrained counter that starts at @c initialSample (usually the
 * sample-clock reading at collection start). Each chunk's @c start_sample
 * is @c initialSample + samplesDrained at chunk open; @c end_sample is
 * @c start_sample + frames_written. If the ReferenceRing reports drops,
 * the writer stamps @c drops_snapshot into the manifest so an audit can
 * see exactly where the sample stream lost sync.
 */
class ContinuousWriter {
public:
    ContinuousWriter(ContinuousWriterConfig cfg,
                     ReferenceRing& ring,
                     const SampleClock& clock);
    ~ContinuousWriter();

    ContinuousWriter(const ContinuousWriter&)            = delete;
    ContinuousWriter& operator=(const ContinuousWriter&) = delete;

    /// Snapshot the current sample-clock value as the anchor for chunk 0,
    /// create the output dir if needed, then spawn the writer thread.
    void start();

    /// Stop the writer thread, close the in-progress chunk cleanly, and
    /// append its manifest record. Idempotent.
    void stop();

    /// Manifest path (informational; used by tests).
    std::filesystem::path manifestPath() const;

    /// Total drops the writer has observed on the ring since @c start().
    /// Populated at each chunk boundary; approximate mid-chunk.
    std::uint64_t droppedTotal() const {
        return m_droppedSnapshot.load(std::memory_order_acquire);
    }

    /// Chunks closed cleanly so far (excluding any currently open chunk).
    std::uint64_t chunksClosed() const {
        return m_chunksClosed.load(std::memory_order_acquire);
    }

private:
    void loop();
    bool openNewChunk();
    void closeCurrentChunk();
    bool appendManifest(const std::string& jsonRecord);

    ContinuousWriterConfig m_cfg;
    ReferenceRing&         m_ring;
    const SampleClock&     m_clock;

    std::atomic<bool>      m_running{false};
    std::thread            m_thread;

    // Chunk-open state. Not shared cross-thread beyond start()/stop()
    // handshake — the loop() worker owns them.
    std::uint64_t          m_initialSample{0};
    std::uint64_t          m_samplesDrained{0};
    std::uint64_t          m_chunkStartSample{0};
    std::chrono::system_clock::time_point m_chunkStartWall{};
    std::filesystem::path  m_currentChunkPath{};
    void*                  m_currentSf{nullptr};       // SNDFILE* (opaque here)
    std::uint64_t          m_currentChunkFrames{0};

    std::atomic<std::uint64_t> m_droppedSnapshot{0};
    std::atomic<std::uint64_t> m_chunksClosed{0};
};

} // namespace echobox::collection
