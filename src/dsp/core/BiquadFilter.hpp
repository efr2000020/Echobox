// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Direct-form-I biquad filter used as the DSP pipeline's pre-FFT high-pass.

#include <cmath>
#include <numbers>

/**
 * @brief Direct-form-I biquad. Header-only and trivially copyable.
 *
 * Coefficients are normalized to @c a0 at @c configure time so @c process()
 * is one multiply-add per coefficient with no per-sample division.
 */
class BiquadFilter {
public:
    BiquadFilter() : x1(0), x2(0), y1(0), y2(0),
                     b0(0), b1(0), b2(0), a1(0), a2(0) {}

    /**
     * @brief Configure for a 2nd-order high-pass response.
     * @param cutoffFreq The -3 dB corner frequency, in Hz.
     * @param sampleRate Stream sample rate, in Hz. No-op on @c sampleRate==0.
     * @param Q          Resonance/quality factor; @c 0.707 (Butterworth) is flat.
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
     * @brief Filter one sample.
     * @return The filtered output for @p x0 given the current state.
     * @note Inlined deliberately — called once per input sample at 384 kHz.
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

    /// Zero the delay lines without touching the coefficients.
    void reset() {
        x1 = x2 = y1 = y2 = 0.0f;
    }

private:
    // Filter coefficients
    float b0, b1, b2, a1, a2;
    // Delay lines (history)
    float x1, x2, y1, y2;
};