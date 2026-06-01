/// @file
/// KissFftEngine implementation. Also defines @c makeFftEngine() — replace
/// this TU (or CMake-gate an alternative like @c FftwFftEngine.cpp) to swap
/// the FFT backend.

#include "KissFftEngine.hpp"
#include "Window.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace echobox::dsp {

KissFftEngine::KissFftEngine(std::size_t hopSize, std::size_t fftSize)
    : m_hopSize(hopSize),
      m_nfft(fftSize == 0 ? hopSize : fftSize),
      m_cfg(nullptr),
      m_historyBuffer(m_nfft, 0.0f),
      m_window(::dsp::Window::createHann(m_nfft)),
      m_windowedInput(m_nfft, 0.0f),
      m_complexOutput(m_nfft / 2 + 1)
{
    m_cfg = kiss_fftr_alloc(static_cast<int>(m_nfft), 0, nullptr, nullptr);
    if (!m_cfg) {
        throw std::runtime_error("KissFftEngine: kiss_fftr_alloc failed");
    }
}

KissFftEngine::~KissFftEngine() {
    if (m_cfg) kiss_fft_free(m_cfg);
}

void KissFftEngine::process(std::span<const float> inputTime,
                            std::span<float>       outputMagnitudes) {
    const std::size_t shiftAmount = m_nfft - m_hopSize;

    if (shiftAmount > 0) {
        std::copy(m_historyBuffer.begin() + m_hopSize,
                  m_historyBuffer.end(),
                  m_historyBuffer.begin());
    }
    std::copy(inputTime.begin(), inputTime.end(),
              m_historyBuffer.begin() + shiftAmount);

    for (std::size_t i = 0; i < m_nfft; ++i) {
        m_windowedInput[i] = m_historyBuffer[i] * m_window[i];
    }

    kiss_fftr(m_cfg, m_windowedInput.data(), m_complexOutput.data());

    const float invN = 1.0f / static_cast<float>(m_nfft);
    for (std::size_t i = 0; i < outputMagnitudes.size(); ++i) {
        const float re = m_complexOutput[i].r;
        const float im = m_complexOutput[i].i;
        outputMagnitudes[i] = std::sqrt(re * re + im * im) * invN;
    }
}

// Backend selection point. Replace this TU (or add a CMake-gated alternative
// like FftwFftEngine.cpp) to swap the backend.
std::unique_ptr<IFftEngine> makeFftEngine(std::size_t hopSize, std::size_t fftSize) {
    return std::make_unique<KissFftEngine>(hopSize, fftSize);
}

} // namespace echobox::dsp
