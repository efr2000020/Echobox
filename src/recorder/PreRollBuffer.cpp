/// @file
/// PreRollBuffer implementation. See PreRollBuffer.hpp for the contract.

#include "PreRollBuffer.hpp"
#include <algorithm>
#include <cstring>

namespace echobox::recorder {

PreRollBuffer::PreRollBuffer(std::size_t capacitySamples)
    : m_capacity(capacitySamples), m_buf(capacitySamples, 0) {}

void PreRollBuffer::write(std::span<const std::int16_t> samples) {
    const std::uint64_t pos = m_writePos.load(std::memory_order_relaxed);
    const std::size_t   n   = samples.size();

    // Two-segment copy across the ring boundary.
    const std::size_t offset = static_cast<std::size_t>(pos % m_capacity);
    const std::size_t first  = std::min(n, m_capacity - offset);
    std::memcpy(m_buf.data() + offset, samples.data(), first * sizeof(std::int16_t));
    if (n > first) {
        std::memcpy(m_buf.data(), samples.data() + first,
                    (n - first) * sizeof(std::int16_t));
    }

    // Publish: release-store the new write position last so a consumer that
    // sees the new position is guaranteed to see the new samples.
    m_writePos.store(pos + n, std::memory_order_release);
}

PreRollBuffer::Cursor PreRollBuffer::openCursor(std::size_t prerollSamples) const {
    const std::uint64_t pos = m_writePos.load(std::memory_order_acquire);
    Cursor c;
    c.valid = true;
    c.pos = (pos > prerollSamples) ? (pos - prerollSamples) : 0;
    return c;
}

std::size_t PreRollBuffer::read(Cursor& cursor, std::span<std::int16_t> dest,
                                std::size_t& outLostSamples) const {
    outLostSamples = 0;
    if (!cursor.valid || dest.empty()) return 0;

    const std::uint64_t writePos = m_writePos.load(std::memory_order_acquire);
    if (cursor.pos >= writePos) return 0;  // nothing new

    // Detect overrun: producer has lapped us by more than capacity. Jump the
    // cursor forward to the oldest still-valid sample and report the drop.
    if (writePos - cursor.pos > m_capacity) {
        const std::uint64_t oldest = writePos - m_capacity;
        outLostSamples = static_cast<std::size_t>(oldest - cursor.pos);
        cursor.pos = oldest;
    }

    const std::size_t available = static_cast<std::size_t>(writePos - cursor.pos);
    const std::size_t toRead    = std::min(available, dest.size());
    const std::size_t offset    = static_cast<std::size_t>(cursor.pos % m_capacity);
    const std::size_t first     = std::min(toRead, m_capacity - offset);
    std::memcpy(dest.data(), m_buf.data() + offset, first * sizeof(std::int16_t));
    if (toRead > first) {
        std::memcpy(dest.data() + first, m_buf.data(),
                    (toRead - first) * sizeof(std::int16_t));
    }

    cursor.pos += toRead;
    return toRead;
}

} // namespace echobox::recorder
