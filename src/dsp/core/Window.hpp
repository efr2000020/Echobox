#pragma once
/// @file
/// Analysis windows for FFT pre-multiplication.

#include <vector>
#include <cmath>
#include <numbers>

namespace dsp {

/// @brief Factory for analysis windows used by FFT engines.
class Window {
public:
    /// Standard symmetric Hann window of length @p size.
    static std::vector<float> createHann(size_t size) {
        std::vector<float> window(size);
        for (size_t i = 0; i < size; ++i) {
            window[i] = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * i / (size - 1)));
        }
        return window;
    }
};

} // namespace dsp