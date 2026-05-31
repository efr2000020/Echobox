#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace echobox::recorder {

/**
 * Always-overwriting circular buffer of int16 audio samples.
 *
 * Designed for the recorder gate use case: one producer (audio thread) writes
 * continuously; one consumer (recorder thread) is mostly idle, then on
 * detection starts reading at a "now minus N samples" cursor and drains
 * forward in real time.
 *
 * When idle the producer simply overruns the consumer's would-be position —
 * the consumer doesn't have a position yet. Once the consumer opens a read
 * cursor and falls more than `capacity()` samples behind the producer, the
 * read() call detects the overrun, jumps the cursor forward to the oldest
 * still-valid sample, and reports the loss to the caller.
 *
 * Thread safety: single producer, single consumer. Producer never blocks.
 * Consumer never blocks the producer.
 */
class PreRollBuffer {
public:
    explicit PreRollBuffer(std::size_t capacitySamples);

    PreRollBuffer(const PreRollBuffer&)            = delete;
    PreRollBuffer& operator=(const PreRollBuffer&) = delete;

    std::size_t capacity() const { return m_capacity; }

    /** Absolute sample count written since construction. Wraps after ~10^14 yrs at 384 kHz. */
    std::uint64_t writeCount() const { return m_writePos.load(std::memory_order_acquire); }

    /** Producer: always succeeds, overwriting the oldest data when full. */
    void write(std::span<const std::int16_t> samples);

    /** Consumer cursor; absolute sample index into the producer's stream. */
    struct Cursor {
        std::uint64_t pos = 0;
        bool valid = false;
    };

    /**
     * Returns a read cursor positioned `prerollSamples` behind the current
     * write head (or at 0 if not that much has been written yet).
     */
    Cursor openCursor(std::size_t prerollSamples) const;

    /**
     * Drains as many samples as are currently available into `dest`, advancing
     * the cursor. If the cursor had fallen more than capacity() behind the
     * write head, samples have been lost: the cursor is jumped forward to the
     * oldest still-valid sample and `outLostSamples` reports the drop count.
     *
     * @return  number of samples written to `dest`.
     */
    std::size_t read(Cursor& cursor, std::span<std::int16_t> dest,
                     std::size_t& outLostSamples) const;

private:
    const std::size_t              m_capacity;
    mutable std::vector<std::int16_t> m_buf;        // mutable: read() is logically const
    std::atomic<std::uint64_t>     m_writePos{0};
};

} // namespace echobox::recorder
