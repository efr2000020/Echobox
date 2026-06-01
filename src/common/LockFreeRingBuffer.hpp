// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Single-producer / single-consumer lock-free ring buffer used to hand
/// audio samples from the capture thread to the DSP thread.

#include <vector>
#include <atomic>
#include <cstddef>
#include <span>

/**
 * @brief Single-producer / single-consumer lock-free ring buffer.
 *
 * Capacity-N constructor allocates @c N+1 slots so the empty/full distinction
 * is unambiguous without a separate count.
 *
 * @warning Exactly one thread may call @c push() and exactly one (different)
 *          thread may call @c pop(). Calling either from multiple threads is
 *          undefined behaviour.
 */
template <typename T>
class LockFreeRingBuffer {
public:
    explicit LockFreeRingBuffer(size_t capacity)
        : m_buffer(capacity + 1), m_capacity(capacity + 1) {}

    /// Producer: enqueue one element. Returns @c false when the buffer is
    /// full (the producer is expected to handle this without blocking).
    bool push(const T& value) {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) % m_capacity;

        if (next_head == m_tail.load(std::memory_order_acquire)) {
            return false; // Buffer full
        }

        m_buffer[head] = value;
        m_head.store(next_head, std::memory_order_release);
        return true;
    }

    /// Consumer: dequeue one element into @p value. Returns @c false when
    /// the buffer is empty.
    bool pop(T& value) {
        size_t tail = m_tail.load(std::memory_order_relaxed);

        if (tail == m_head.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }

        value = m_buffer[tail];
        m_tail.store((tail + 1) % m_capacity, std::memory_order_release);
        return true;
    }

    /// Number of elements currently enqueued. Approximate from either side:
    /// the producer may push or the consumer may pop concurrently with this
    /// call.
    size_t available_read() const {
        size_t head = m_head.load(std::memory_order_acquire);
        size_t tail = m_tail.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (m_capacity - tail + head);
    }

private:
    std::vector<T> m_buffer;
    size_t m_capacity;
    alignas(64) std::atomic<size_t> m_head{0}; // Align to cache line to prevent false sharing
    alignas(64) std::atomic<size_t> m_tail{0};
};