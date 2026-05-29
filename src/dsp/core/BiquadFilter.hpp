#pragma once
#include <cmath>
#include <numbers>

class BiquadFilter {
public:
    BiquadFilter() : x1(0), x2(0), y1(0), y2(0), 
                     b0(0), b1(0), b2(0), a1(0), a2(0) {}

    /**
     * @brief Configures the coefficients for a High-Pass Filter.
     * @param cutoffFreq The frequency to cut off at (e.g., 20000.0f).
     * @param sampleRate The current audio stream sample rate.
     * @param Q The resonance/quality factor. 0.707 (Butterworth) provides a flat response.
     */
    void configureHighPass(float cutoffFreq, float sampleRate, float Q = 0.707f) {
        if (sampleRate == 0) return;

        float w0 = 2.0f * std::numbers::pi_v<float> * cutoffFreq / sampleRate;
        float alpha = std::sin(w0) / (2.0f * Q);
        float cosw0 = std::cos(w0);

        float a0 = 1.0f + alpha;
        
        // Normalize all coefficients by a0 to save a division step during processing
        b0 = ((1.0f + cosw0) / 2.0f) / a0;
        b1 = -(1.0f + cosw0) / a0;
        b2 = ((1.0f + cosw0) / 2.0f) / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;
        
        reset();
    }

    /**
     * @brief Processes a single float sample. Marked inline for maximum performance.
     */
    inline float process(float x0) {
        // The core difference equation
        float y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        
        // Shift the state delays forward
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
        
        return y0;
    }

    void reset() {
        x1 = x2 = y1 = y2 = 0.0f;
    }

private:
    // Filter coefficients
    float b0, b1, b2, a1, a2;
    // Delay lines (history)
    float x1, x2, y1, y2;
};