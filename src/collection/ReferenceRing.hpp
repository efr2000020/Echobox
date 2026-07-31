// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Stream-A audio tap: SPSC int16 ring the audio thread pushes into and
/// the continuous writer thread drains. Drops are counted and reported —
/// the plan (§2.2 A, §3) treats any drop as a session-invalidating event
/// that the offline integrity checks must catch.

#include "common/LockFreeRingBuffer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace echobox::collection {

/**
 * @brief Wait-free int16 ring for Stream A + a drop counter.
 *
 * @c pushBatch is called from the audio thread. It never blocks and never
 * allocates. When the ring is full it increments @c dropped() by the number
 * of samples it could not enqueue — the sample clock still advances by the
 * full batch (drops are lost audio, not lost time), so downstream tools
 * can detect the gap via @c dropped() + the sample clock.
 *
 * @c popOne is called from the writer thread. Any thread may call
 * @c dropped() to observe the drop count (release/acquire ordering on the
 * underlying atomic).
 *
 * @note Sizing guidance: at 384 kHz mono, 8 seconds ≈ 6 MiB. Comfortable
 *       headroom for SD-card fsync stalls without inviting drops.
 */
class ReferenceRing {
public:
    explicit ReferenceRing(std::size_t capacitySamples)
        : m_ring(capacitySamples) {}

    ReferenceRing(const ReferenceRing&)            = delete;
    ReferenceRing& operator=(const ReferenceRing&) = delete;

    /// Producer (audio thread): enqueue @p samples. Returns the number of
    /// samples DROPPED (ring full) so the caller can also emit a per-batch
    /// warning if it likes. @c dropped() is incremented by the same number.
    std::size_t pushBatch(std::span<const std::int16_t> samples) {
        std::size_t dropped = 0;
        for (auto s : samples) {
            if (!m_ring.push(s)) ++dropped;
        }
        if (dropped > 0) {
            m_dropped.fetch_add(dropped, std::memory_order_release);
        }
        return dropped;
    }

    /// Consumer (writer thread): dequeue one sample. Returns false if the
    /// ring is empty.
    bool popOne(std::int16_t& out) { return m_ring.pop(out); }

    /// Consumer: total drops observed by the producer so far. Monotonic.
    std::uint64_t dropped() const {
        return m_dropped.load(std::memory_order_acquire);
    }

    /// Approximate samples enqueued but not yet drained. Cheap; both sides
    /// may call it. Only exact from the perspective of the caller's own
    /// side of the ring.
    std::size_t availableRead() const { return m_ring.available_read(); }

private:
    LockFreeRingBuffer<std::int16_t> m_ring;
    std::atomic<std::uint64_t>       m_dropped{0};
};

} // namespace echobox::collection
