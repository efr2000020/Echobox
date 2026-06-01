// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dsp/core/BiquadFilter.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

TEST_CASE("BiquadFilter: HighPass Rejection", "[dsp]") {
    BiquadFilter filter;
    const float sampleRate = 384000.0f;
    const float cutoff = 20000.0f;
    filter.configureHighPass(cutoff, sampleRate);

    SECTION("DC Rejection") {
        // A high-pass filter should eventually output 0 for a constant DC input
        float output = 0;
        for (int i = 0; i < 1000; ++i) {
            output = filter.process(1.0f);
        }
        CHECK_THAT(output, Catch::Matchers::WithinAbs(0.0f, 1e-4f));
    }

    SECTION("Passband transparency") {
        // Frequency well above cutoff should pass with minimal attenuation
        const float freq = 100000.0f;
        const float dt = 1.0f / sampleRate;
        
        // Let it settle
        for (int i = 0; i < 1000; ++i) {
            filter.process(std::sin(2.0f * 3.14159f * freq * i * dt));
        }

        float inputSqSum = 0;
        float outputSqSum = 0;
        const int n = 1000;
        for (int i = 1000; i < 1000 + n; ++i) {
            float input = std::sin(2.0f * 3.14159f * freq * i * dt);
            float output = filter.process(input);
            inputSqSum += input * input;
            outputSqSum += output * output;
        }
        
        float inputRms = std::sqrt(inputSqSum / n);
        float outputRms = std::sqrt(outputSqSum / n);
        
        // Gain should be close to 1.0 (within 10% to be safe, but usually much closer)
        CHECK_THAT(outputRms / inputRms, Catch::Matchers::WithinAbs(1.0f, 0.1f));
    }

    SECTION("Reset") {
        for (int i = 0; i < 100; ++i) filter.process(1.0f);
        filter.reset();
        // After reset, first output for DC 1.0 should be exactly b0
        // because x1, x2, y1, y2 are 0.
        // y0 = b0 * 1.0 + b1 * 0 + b2 * 0 - a1 * 0 - a2 * 0 = b0
        // Wait, b0 is normalized.
        // Actually, just check that it's different from the settled 0.
        CHECK(filter.process(1.0f) > 0.1f);
    }
}

TEST_CASE("BiquadFilter: Impulse Response", "[dsp]") {
    BiquadFilter filter;
    filter.configureHighPass(1000.0f, 10000.0f); // Cutoff 0.1 * fs

    // Impulse at t=0
    float y0 = filter.process(1.0f);
    float y1 = filter.process(0.0f);
    float y2 = filter.process(0.0f);

    // Filter shouldn't be dead
    CHECK(y0 != 0.0f);
    CHECK(y1 != 0.0f);
    CHECK(y2 != 0.0f);
}
