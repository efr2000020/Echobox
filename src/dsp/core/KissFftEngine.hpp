#pragma once
#include "IFftEngine.hpp"
#include <kiss_fftr.h>
#include <cstddef>
#include <span>
#include <vector>

namespace litespec::dsp {

/**
 * IFftEngine implementation backed by KissFFT.
 *
 * Pre-allocates the overlap history, window, scratch and complex output
 * buffers; process() does no allocations and no I/O.
 */
class KissFftEngine final : public IFftEngine {
public:
    KissFftEngine(std::size_t hopSize, std::size_t fftSize);
    ~KissFftEngine() override;

    KissFftEngine(const KissFftEngine&)            = delete;
    KissFftEngine& operator=(const KissFftEngine&) = delete;

    std::size_t getFFTSize()  const override { return m_nfft; }
    std::size_t getHopSize()  const override { return m_hopSize; }
    std::size_t getNumBins()  const override { return m_nfft / 2 + 1; }

    void process(std::span<const float> inputTime,
                 std::span<float>       outputMagnitudes) override;

private:
    std::size_t                m_hopSize;
    std::size_t                m_nfft;
    kiss_fftr_cfg              m_cfg;
    std::vector<float>         m_historyBuffer;
    std::vector<float>         m_window;
    std::vector<float>         m_windowedInput;
    std::vector<kiss_fft_cpx>  m_complexOutput;
};

} // namespace litespec::dsp
