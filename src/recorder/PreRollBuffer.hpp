#pragma once
/// @file
/// Always-overwriting pre-roll buffer feeding the recorder's lead-in.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace echobox::recorder {

/**
 * @brief Always-overwriting circular buffer of @c int16 audio samples.
 *
 * Designed for the recorder-gate use case: one producer (the audio thread)
 * writes continuously; one consumer (the recorder thread) is mostly idle,
 * then on detection opens a "now minus N samples" cursor and drains forward
 * in real time.
 *
 * When idle the producer simply overruns the consumer's would-be position —
 * the consumer doesn't have a position yet. Once the consumer opens a read
 * cursor and falls more than @c capacity() samples behind the producer, the
 * @c read() call detects the overrun, jumps the cursor forward to the
 * oldest still-valid sample, and reports the loss to the caller.
 *
 * @note Thread model: single producer, single consumer. The producer never
 *       blocks. The consumer never blocks the producer.
 */
class PreRollBuffer {
public:
    /// @param capacitySamples Ring capacity in @c int16 samples (not frames).
    explicit PreRollBuffer(std::size_t capacitySamples);

    PreRollBuffer(const PreRollBuffer&)            = delete;
    PreRollBuffer& operator=(const PreRollBuffer&) = delete;

    std::size_t capacity() const { return m_capacity; }

    /// Absolute sample count written since construction.
    /// (Wraps after ~10^14 years at 384 kHz.)
    std::uint64_t writeCount() const { return m_writePos.load(std::memory_order_acquire); }

    /// Producer write. Always succeeds; overwrites the oldest data when full.
    void write(std::span<const std::int16_t> samples);

    /// Consumer cursor; an absolute sample index into the producer's stream.
    struct Cursor {
        std::uint64_t pos = 0;
        bool valid = false;
    };

    /**
     * @brief Open a cursor positioned @p prerollSamples behind the write head.
     * @return Cursor at @c writeCount-prerollSamples, or at @c 0 if less than
     *         that has been written yet.
     */
    Cursor openCursor(std::size_t prerollSamples) const;

    /**
     * @brief Drain currently-available samples into @p dest.
     *
     * Advances @p cursor by the number of samples returned. If the cursor had
     * fallen more than @c capacity() behind the write head, samples have been
     * lost: the cursor is jumped forward to the oldest still-valid sample and
     * @p outLostSamples reports the drop count.
     *
     * @return Number of samples written to @p dest.
     */
    std::size_t read(Cursor& cursor, std::span<std::int16_t> dest,
                     std::size_t& outLostSamples) const;

private:
    const std::size_t              m_capacity;
    mutable std::vector<std::int16_t> m_buf;        // mutable: read() is logically const
    std::atomic<std::uint64_t>     m_writePos{0};
};

} // namespace echobox::recorder
