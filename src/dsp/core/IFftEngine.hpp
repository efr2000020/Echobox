#pragma once
#include <cstddef>
#include <memory>
#include <span>

namespace litespec::dsp {

/**
 * STFT engine interface.
 *
 * Implementations maintain their own overlap history: each process() consumes
 * exactly hopSize new time-domain samples and emits magnitudes for the
 * fftSize-wide window ending at the new samples.
 *
 * Real-time safety: process() MUST be allocation-free and non-blocking.
 *
 * Backends are selected at build time. Exactly one implementation TU in the
 * dsp_core target defines makeFftEngine().
 */
class IFftEngine {
public:
    virtual ~IFftEngine() = default;

    virtual std::size_t getFFTSize() const = 0;
    virtual std::size_t getHopSize() const = 0;
    virtual std::size_t getNumBins() const = 0;  // == fftSize/2 + 1

    /**
     * @param inputTime         hopSize new time-domain samples.
     * @param outputMagnitudes  numBins normalized magnitudes (|X|/fftSize).
     */
    virtual void process(std::span<const float> inputTime,
                         std::span<float>       outputMagnitudes) = 0;
};

/** Factory for the build-selected FFT backend. */
std::unique_ptr<IFftEngine> makeFftEngine(std::size_t hopSize, std::size_t fftSize);

} // namespace litespec::dsp
