#pragma once
#include <vector>
#include <cmath>
#include <numbers>

namespace dsp {

class Window {
public:
    static std::vector<float> createHann(size_t size) {
        std::vector<float> window(size);
        for (size_t i = 0; i < size; ++i) {
            window[i] = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * i / (size - 1)));
        }
        return window;
    }
};

} // namespace dsp