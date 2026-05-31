#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace echobox::audio {

/**
 * Capture-side audio source abstraction.
 *
 * Samples are returned as native int16, mono interleaved (channels()==1 is
 * the only configuration we support in production; the interface is shaped
 * to allow multichannel in future without a breaking change).
 *
 * The owning thread calls open() once, then read() in a loop until close().
 * Implementations are expected to block read() for up to a short timeout
 * (~100 ms) when no samples are immediately available, returning 0 in that
 * case so the caller can re-check its shutdown flag.
 */
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    /** Opens the underlying device. Throws on failure. */
    virtual void open() = 0;

    /** Closes the device. Idempotent. */
    virtual void close() noexcept = 0;

    virtual int sampleRate() const = 0;
    virtual int channels()   const = 0;

    /** Human-readable identifier for log lines. */
    virtual const std::string& name() const = 0;

    /**
     * @return  > 0  number of int16 samples written to `dest`.
     *         == 0  no samples this call (timeout); not an error.
     *          < 0  fatal: source is no longer usable; caller should stop.
     */
    virtual int read(std::span<std::int16_t> dest) = 0;
};

} // namespace echobox::audio
