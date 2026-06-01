#pragma once
/// @file
/// STFT engine interface plus the build-time backend factory.

#include <cstddef>
#include <memory>
#include <span>

namespace echobox::dsp {

/**
 * @brief Hop-aligned short-time Fourier transform engine.
 *
 * Implementations maintain their own overlap history: each @c process() call
 * consumes exactly @c hopSize new time-domain samples and emits magnitudes
 * for the @c fftSize-wide window ending at those new samples.
 *
 * @warning @c process() MUST be allocation-free and non-blocking. It runs on
 *          the DSP thread, hop-cadenced (~1.3 ms at 384 kHz / hop=512).
 *
 * @note Backends are selected at build time: exactly one implementation TU
 *       in the @c dsp_core target defines @c makeFftEngine().
 */
class IFftEngine {
public:
    virtual ~IFftEngine() = default;

    virtual std::size_t getFFTSize() const = 0;
    virtual std::size_t getHopSize() const = 0;
    /// FFT bin count exposed to consumers. Equals @c fftSize/2+1.
    virtual std::size_t getNumBins() const = 0;

    /**
     * @brief Push one hop and produce the matching magnitude frame.
     * @param inputTime        Exactly @c hopSize new time-domain samples.
     * @param outputMagnitudes Exactly @c numBins normalized magnitudes
     *                         (@c |X|/fftSize).
     */
    virtual void process(std::span<const float> inputTime,
                         std::span<float>       outputMagnitudes) = 0;
};

/// Build-selected FFT backend factory. Defined by exactly one backend TU.
std::unique_ptr<IFftEngine> makeFftEngine(std::size_t hopSize, std::size_t fftSize);

} // namespace echobox::dsp
