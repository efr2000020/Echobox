#pragma once

#include <vector>
#include <atomic>
#include <cstddef>
#include <span>

/**
 * @brief A Single-Producer Single-Consumer Lock-Free Ring Buffer.
 * Designed for real-time audio data handover.
 */
template <typename T>
class LockFreeRingBuffer {
public:
    explicit LockFreeRingBuffer(size_t capacity)
        : m_buffer(capacity + 1), m_capacity(capacity + 1) {}

    // Producers: Write data to the buffer
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

    // Consumers: Read data from the buffer
    bool pop(T& value) {
        size_t tail = m_tail.load(std::memory_order_relaxed);

        if (tail == m_head.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }

        value = m_buffer[tail];
        m_tail.store((tail + 1) % m_capacity, std::memory_order_release);
        return true;
    }

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