// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Sweep-shape feature + cricket-gate tests. The pure helper
/// BandEnergyDetector::computeSweepShape() is tested directly so the
/// classification cases (smooth FM sweep vs. harmonic hopping vs. flat
/// narrowband vs. flat broadband vs. NYCLEI-like) are deterministic and
/// don't require driving the full hot-loop. Detector-level cases drive
/// the gate through processFrame() so the provisional-then-confirm
/// timing, tunable defaults, and manifest lock-step are exercised too.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/algorithms/BandEnergyDetector/BandEnergyDetector.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
using SweepShape = BandEnergyDetector::SweepShape;

namespace {

// Build a synthetic magnitude spectrum with a Gaussian peak centred at
// `peakBin` with a half-power half-width of `halfWidthBins` bins.
// Used to produce dominant-frame magnitude snapshots whose 10-dB bandwidth
// the helper can walk in a predictable way.
//
// NOTE: computeSweepShape's 5th argument is `anchorBin` — the walk's origin
// AND the source of its reference magnitude (see the A1 fix). Pass the same
// bin the Gaussian is centred on, i.e. what the detector would supply as the
// winning sub-band's dominant bin. Passing anything else measures the decay
// tail from an off-peak origin and reports a meaningless width (0 when the
// tail has underflowed to zero).
std::vector<float> gaussianPeak(std::size_t numBins, std::size_t peakBin,
                                float halfWidthBins, float peakMag) {
    std::vector<float> mags(numBins, 0.0f);
    // sigma chosen so amplitude drops by sqrt(10) at +/- halfWidthBins
    //   exp(-x^2 / (2*sigma^2)) = 1/sqrt(10)
    //   x = halfWidthBins  =>  sigma = halfWidthBins / sqrt(ln(10))
    const float sigma = halfWidthBins / std::sqrt(std::log(10.0f));
    for (std::size_t i = 0; i < numBins; ++i) {
        const float dx = static_cast<float>(i) - static_cast<float>(peakBin);
        mags[i] = peakMag * std::exp(-(dx * dx) / (2.0f * sigma * sigma));
    }
    return mags;
}

} // namespace


TEST_CASE("computeSweepShape: smooth ascending FM sweep", "[sweep][shape]") {
    // Ten frames, dominant bin marches up 4 bins/frame; bin resolution 100 Hz.
    // Expected: path_ratio == 1 (no backtracking), mono_fraction == 1,
    // drift = (40 - 0) * 100 Hz = 4 kHz.
    const std::size_t dom[] = {100, 104, 108, 112, 116, 120, 124, 128, 132, 140};
    const auto mags = gaussianPeak(2049, 140, 2.0f, 1.0f);  // narrow peak
    SweepShape s = BandEnergyDetector::computeSweepShape(
        dom, 10, mags.data(), mags.size(), 140, 0, 2049, 100.0f);
    CHECK_THAT(s.drift_khz,     WithinAbs(4.0f, 1e-4f));
    CHECK_THAT(s.path_ratio,    WithinAbs(1.0f, 1e-4f));
    CHECK_THAT(s.mono_fraction, WithinAbs(1.0f, 1e-4f));
}


TEST_CASE("computeSweepShape: harmonic hopping rejects the sweep clause",
          "[sweep][shape]") {
    // Cricket-like dominant-bin pattern: jumps between a fundamental and
    // its second harmonic on every frame. Total |Δ| / range is ~2 (every
    // step covers the whole range), and steps split 50/50 between up/down
    // so mono_fraction is ~0.5 — both fail the sweep clause.
    const std::size_t dom[] = {100, 200, 100, 200, 100, 200, 100, 200};
    const auto mags = gaussianPeak(2049, 100, 1.0f, 1.0f);
    SweepShape s = BandEnergyDetector::computeSweepShape(
        dom, 8, mags.data(), mags.size(), 100, 0, 2049, 100.0f);
    CHECK(s.path_ratio > 1.6f);
    CHECK(s.mono_fraction <= 0.6f);
}


TEST_CASE("computeSweepShape: narrowband flat fails the bandwidth clause",
          "[sweep][shape]") {
    // Dominant bin doesn't move. Peak in the magnitude snapshot is narrower
    // than one bin (0.5 bins half-width), so both immediate neighbours are
    // already below refMag/sqrt(10) and the walk terminates at the anchor
    // itself => 0 bins wide.
    const std::size_t dom[] = {100, 100, 100, 100, 100, 100};
    const auto mags = gaussianPeak(2049, 100, 0.5f, 1.0f);
    SweepShape s = BandEnergyDetector::computeSweepShape(
        dom, 6, mags.data(), mags.size(), 100, 0, 2049, 100.0f);
    CHECK_THAT(s.drift_khz, WithinAbs(0.0f, 1e-6f));
    CHECK(s.bandwidth_khz < 1.1f);
}


TEST_CASE("computeSweepShape: broadband flat passes the bandwidth clause",
          "[sweep][shape]") {
    // Dominant bin doesn't move, but the peak in the magnitude snapshot is
    // wide (~12 bins half-width at 100 Hz/bin => ~2.4 kHz 10-dB BW), well
    // above the 1.1 kHz floor.
    const std::size_t dom[] = {100, 100, 100, 100, 100, 100};
    const auto mags = gaussianPeak(2049, 100, 12.0f, 1.0f);
    SweepShape s = BandEnergyDetector::computeSweepShape(
        dom, 6, mags.data(), mags.size(), 100, 0, 2049, 100.0f);
    CHECK(s.bandwidth_khz >= 1.1f);
}


TEST_CASE("computeSweepShape: mid-bandwidth low-drift stays above the "
          "sweep-gate bandwidth floor",
          "[sweep][shape]") {
    // Pure feature-arithmetic check: for a Gaussian peak with ~1.5 kHz
    // 10-dB bandwidth, computeSweepShape reports at least the shipping
    // bandwidth floor (0.9 kHz — see
    // BandEnergyDetector::m_minBandwidthKhz). Live-clip narrow-band
    // species preservation is validated end-to-end via the
    // recorder-model harness, not against a synthetic Gaussian.
    const std::size_t dom[] = {300, 301, 302, 303, 304, 305};
    const auto mags = gaussianPeak(2049, 305, 7.5f, 1.0f);
    SweepShape s = BandEnergyDetector::computeSweepShape(
        dom, 6, mags.data(), mags.size(), 305, 0, 2049, 100.0f);
    CHECK(s.bandwidth_khz >= 0.9f);
    CHECK(s.drift_khz < 8.0f);
}


TEST_CASE("computeSweepShape: empty ring is safe and returns zeros",
          "[sweep][shape][edge]") {
    const auto mags = gaussianPeak(2049, 100, 1.0f, 1.0f);
    SweepShape s = BandEnergyDetector::computeSweepShape(
        nullptr, 0, mags.data(), mags.size(), 100, 0, 2049, 100.0f);
    CHECK(s.bandwidth_khz == 0.0f);
    CHECK(s.drift_khz     == 0.0f);
    CHECK(s.path_ratio    == 0.0f);
    CHECK(s.mono_fraction == 0.0f);
}


// --- Detector-level integration: gate squelches outState.active ----------

namespace {

// Warm the detector's noise-floor EMA on low-amplitude broadband noise.
std::uint32_t warmFloor(BandEnergyDetector& det, std::size_t nBins,
                        std::uint32_t startFrame, int nFrames,
                        std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(1e-4f, 1e-3f);
    std::vector<float> mags(nBins);
    DetectorState state{};
    Annotation    ann{};
    std::uint32_t f = startFrame;
    for (int i = 0; i < nFrames; ++i, ++f) {
        for (auto& v : mags) v = dist(rng);
        det.processFrame(mags, f, state, ann);
    }
    return f;
}

// Drive 20 hot frames where the dominant bin hops between two bins
// (a "cricket" pattern), then enough silence to close out the event.
// Returns the count of frames that were reported active in outState.
int runHoppingEvent(BandEnergyDetector& det, std::size_t nBins,
                    std::uint32_t& frame, std::mt19937& rng) {
    std::uniform_real_distribution<float> bg(1e-4f, 1e-3f);
    std::vector<float> mags(nBins);
    DetectorState state{};
    Annotation    ann{};
    int activeFrames = 0;
    // 20 hot frames alternating between two narrow peaks 100 bins apart;
    // path_ratio comes out ~2 and mono_fraction ~0.5 — the sweep clause
    // fails. Both peaks are narrow so the bandwidth clause also fails.
    for (int i = 0; i < 20; ++i, ++frame) {
        for (auto& v : mags) v = bg(rng);
        const std::size_t peak = (i % 2 == 0) ? 300 : 400;
        for (std::size_t b = peak; b < peak + 3 && b < nBins; ++b) {
            mags[b] = 0.5f;
        }
        det.processFrame(mags, frame, state, ann);
        if (state.active) ++activeFrames;
    }
    // Silence to close the event.
    for (int i = 0; i < 20; ++i, ++frame) {
        for (auto& v : mags) v = bg(rng);
        det.processFrame(mags, frame, state, ann);
    }
    return activeFrames;
}

} // namespace


TEST_CASE("processFrame: sweep gate suppresses a hopping cricket event",
          "[sweep][detector]") {
    constexpr int          sampleRate = 384000;
    constexpr std::size_t  fftSize    = 4096;
    const std::size_t      nBins      = fftSize / 2 + 1;

    BandEnergyDetector det;
    det.configure(sampleRate, fftSize, 20000.0f, 192000.0f);
    REQUIRE(det.setTunable("sweep_gate_enabled", 1.0));

    std::mt19937 rng(0xC0FFEE);
    std::uint32_t frame = 0;
    frame = warmFloor(det, nBins, frame, 200, rng);

    const int activeFrames = runHoppingEvent(det, nBins, frame, rng);

    // Provisional-then-confirm: a few frames at the leading edge will be
    // reported active before the gate fills its ring and rejects. After
    // that, active must drop. The exact count depends on GATE_DECISION_FRAMES
    // and min_active_frames, but it MUST be substantially less than the 20
    // hot frames we drove — otherwise the gate isn't doing anything.
    CHECK(activeFrames < 20);
    CHECK(activeFrames <= 10);  // ~6 decision frames + a couple of slack

    // Drain the sidecar payload; the rejected event should still appear
    // there (so the validator can see what was suppressed) with
    // gate_rejected=true.
    ::SidecarPayload payload;
    REQUIRE(det.drainSidecarPayload(payload));
    REQUIRE_FALSE(payload.events.empty());
    CHECK(payload.events.front().gate_rejected == true);
    CHECK(payload.events.front().drift_khz > 0.0f);
    CHECK(payload.events.front().path_ratio > 1.0f);
}


TEST_CASE("processFrame: sweep gate off is a no-op vs pre-gate behaviour",
          "[sweep][detector]") {
    // With the gate explicitly disabled, the same hopping event should
    // remain reported active for most of its hot frames (subject to the
    // existing flatness + SNR gates). This is the regression guard that
    // sweep_gate_enabled=0 restores byte-identical pre-gate behaviour,
    // so a field deployment can flip the gate off with a single tunable
    // if it produces bat calls the gate cannot characterise.
    constexpr int          sampleRate = 384000;
    constexpr std::size_t  fftSize    = 4096;
    const std::size_t      nBins      = fftSize / 2 + 1;

    BandEnergyDetector det;
    det.configure(sampleRate, fftSize, 20000.0f, 192000.0f);
    REQUIRE(det.setTunable("sweep_gate_enabled", 0.0));

    // Sanity: the ship default is ON. The kill-switch is a supported
    // override, not the default.
    BandEnergyDetector det2;
    det2.configure(sampleRate, fftSize, 20000.0f, 192000.0f);
    double gateVal = -1.0;
    REQUIRE(det2.getTunable("sweep_gate_enabled", &gateVal));
    CHECK(gateVal == 1.0);

    std::mt19937 rng(0xC0FFEE);
    std::uint32_t frame = 0;
    frame = warmFloor(det, nBins, frame, 200, rng);

    const int activeFrames = runHoppingEvent(det, nBins, frame, rng);
    // With the gate off, every passing-the-existing-gates hot frame should
    // be reported active. We expect more than half of the 20 hot frames.
    CHECK(activeFrames > 10);
}


TEST_CASE("tunable manifest lists exactly the keys set/getTunable accept",
          "[sweep][tunables][lockstep]") {
    // Drift between listTunables(), setTunable() and getTunable() silently
    // hides a knob from the GUI/validator. Enumerate the manifest and verify
    // each key round-trips via set+get.
    BandEnergyDetector det;
    det.configure(384000, 4096, 20000.0f, 192000.0f);
    for (const auto& t : det.listTunables()) {
        double v = -1.0;
        REQUIRE(det.getTunable(t.key, &v));
        REQUIRE(det.setTunable(t.key, t.default_value));
    }
    // Spot-check that the 5 new sweep-gate keys are present.
    for (const char* k : {"sweep_gate_enabled", "min_bandwidth_khz",
                          "sweep_drift_khz", "sweep_path_ratio_max",
                          "sweep_mono_frac_min"}) {
        double v = -1.0;
        CHECK(det.getTunable(k, &v));
    }
}
