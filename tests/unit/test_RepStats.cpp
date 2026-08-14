// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Repetition-rate helper + temporal-guard tests. The pure helper
/// BandEnergyDetector::computeRepStats() is tested directly so the
/// metronomic/bursty separation is deterministic and independent of the
/// sweep-shape gate; the detector-level cases exercise the composed veto
/// (sweep + temporal) end-to-end.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/algorithms/BandEnergyDetector/BandEnergyDetector.hpp"

#include <cstdint>
#include <cstddef>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
using RepStats = BandEnergyDetector::RepStats;


TEST_CASE("computeRepStats: metronomic cricket train has low CV(IDI)",
          "[rep][temporal]") {
    // Onsets spaced 100 frames apart at a 750 fps frame rate: rate = 7.5 Hz,
    // sits inside the [4, 20] cricket band; CV(IDI) = 0. This is the pattern
    // the temporal guard must classify as "metronomic".
    const std::uint32_t onsets[] = {0, 100, 200, 300, 400, 500};
    RepStats r = BandEnergyDetector::computeRepStats(onsets, 6, 750.0f);
    CHECK(r.n == 6);
    CHECK_THAT(r.rate_hz, WithinAbs(7.5f, 0.01f));
    CHECK(r.cv_idi < 0.05f);
}


TEST_CASE("computeRepStats: bursty bat pass has high CV(IDI)",
          "[rep][temporal]") {
    // Real bat passes have irregular IDIs (a burst of calls followed by a
    // gap). Rate ≈ 1.5/s, CV(IDI) ≥ 0.5 — the bursty side of the guard.
    // Onsets: 0, 15, 30, 55, 400 frames (calls in a burst, then a pause).
    const std::uint32_t onsets[] = {0, 15, 30, 55, 400};
    RepStats r = BandEnergyDetector::computeRepStats(onsets, 5, 750.0f);
    CHECK(r.n == 5);
    CHECK(r.cv_idi > 0.5f);
}


TEST_CASE("computeRepStats: insufficient history returns count only",
          "[rep][temporal][edge]") {
    const std::uint32_t onsets[] = {0};
    RepStats r = BandEnergyDetector::computeRepStats(onsets, 1, 750.0f);
    CHECK(r.n == 1);
    CHECK(r.rate_hz == 0.0f);
    CHECK(r.cv_idi  == 0.0f);
    // Two onsets: enough for one interval, so rate is defined and CV is 0.
    const std::uint32_t two[] = {0, 100};
    r = BandEnergyDetector::computeRepStats(two, 2, 750.0f);
    CHECK(r.n == 2);
    CHECK_THAT(r.rate_hz, WithinAbs(7.5f, 0.01f));
    CHECK(r.cv_idi == 0.0f);
}


TEST_CASE("computeRepStats: null / zero frame-rate is safe", "[rep][edge]") {
    RepStats r = BandEnergyDetector::computeRepStats(nullptr, 0, 750.0f);
    CHECK(r.n == 0);
    r = BandEnergyDetector::computeRepStats(nullptr, 4, 750.0f);
    CHECK(r.n == 0);
    const std::uint32_t onsets[] = {0, 100, 200};
    r = BandEnergyDetector::computeRepStats(onsets, 3, 0.0f);
    CHECK(r.rate_hz == 0.0f);
}


TEST_CASE("processFrame: temporal veto still fires after the onset ring "
          "wraps past ONSET_RING_CAP — regression test",
          "[rep][detector][regression]") {
    // The onset ring is circular but was once handed to computeRepStats
    // as a linear array [0..count-1]. After >ONSET_RING_CAP events the
    // tail of the linear array holds newer entries than the head; reading
    // linearly puts the sequence out of temporal order, trips the
    // unsorted-onset safety guard, and silently returns n=0 — the
    // temporal veto then never fires on any file with more than
    // ONSET_RING_CAP detected events (i.e. every busy cricket file).
    // Detector must survive a wrap without dropping n.
    //
    // We drive a synthetic metronomic spectrogram directly through
    // processFrame() and confirm >0 events get gate_rejected via the
    // temporal path.
    constexpr int         sampleRate = 384000;
    constexpr std::size_t fftSize    = 4096;
    const std::size_t     nBins      = fftSize / 2 + 1;

    BandEnergyDetector det;
    det.configure(sampleRate, fftSize, 20000.0f, 192000.0f);
    REQUIRE(det.setTunable("sweep_gate_enabled", 1.0));
    REQUIRE(det.setTunable("rep_guard_enabled",  1.0));
    // Encourage the sweep gate to accept — we want to exercise the temporal
    // path, not the sweep path.
    REQUIRE(det.setTunable("min_bandwidth_khz",  0.5));

    // Feed a low-amplitude broadband warmup so the noise floor converges,
    // then a metronomic sequence of 30 narrowband events spaced ~100 frames
    // apart (13.3 Hz — inside the cricket rate band). >16 events forces a
    // wrap through ONSET_RING_CAP.
    std::mt19937 rng(0xBEEF);
    std::uniform_real_distribution<float> bg(1e-4f, 1e-3f);
    std::vector<float> mags(nBins);
    DetectorState state{};
    Annotation    ann{};
    std::uint32_t frame = 0;
    for (int i = 0; i < 200; ++i, ++frame) {
        for (auto& v : mags) v = bg(rng);
        det.processFrame(mags, frame, state, ann);
    }

    int emitted = 0;
    for (int e = 0; e < 30; ++e) {
        // 4 hot frames per event, then 96 silent — 100 frames spacing.
        for (int i = 0; i < 4; ++i, ++frame) {
            for (auto& v : mags) v = bg(rng);
            for (std::size_t b = 300; b < 305 && b < nBins; ++b) mags[b] = 0.5f;
            if (det.processFrame(mags, frame, state, ann)) ++emitted;
        }
        for (int i = 0; i < 96; ++i, ++frame) {
            for (auto& v : mags) v = bg(rng);
            if (det.processFrame(mags, frame, state, ann)) ++emitted;
        }
    }

    // Drain the sidecar and count how many events were gate-rejected via
    // the temporal path — must be > 0 after the wrap, otherwise the ring
    // ordering is broken again.
    ::SidecarPayload payload;
    REQUIRE(det.drainSidecarPayload(payload));
    int rejected = 0;
    for (const auto& e : payload.events) {
        if (e.gate_rejected) ++rejected;
    }
    // If the bug reappears, `emitted` will be ~30 (every event kept) and
    // `rejected` will be tiny (only the first 16 events could ever veto).
    // With the ring-order fix, most of the 30 events get vetoed.
    CHECK(rejected > 10);
}


TEST_CASE("computeRepStats: unsorted onsets are rejected instead of "
          "returning nonsense",
          "[rep][edge]") {
    // A caller passing an unsorted ring is a bug; better to signal
    // insufficient history than return a stat computed on wrap-around
    // differences that fluke past the metronomic threshold.
    const std::uint32_t bad[] = {100, 50, 150, 200};
    RepStats r = BandEnergyDetector::computeRepStats(bad, 4, 750.0f);
    CHECK(r.n == 0);
}


TEST_CASE("tunable manifest carries the temporal-guard keys",
          "[rep][tunables][lockstep]") {
    // Lock-step check between listTunables() and get/setTunable(): the
    // temporal guard exposes six knobs plus hop_size_samples. Every one
    // must round-trip via set+get and appear in the manifest — else the
    // GUI form silently hides them.
    BandEnergyDetector det;
    det.configure(384000, 4096, 20000.0f, 192000.0f);

    // Every manifest entry round-trips.
    for (const auto& t : det.listTunables()) {
        double v = -1.0;
        REQUIRE(det.getTunable(t.key, &v));
        REQUIRE(det.setTunable(t.key, t.default_value));
    }

    // Spot-check the new keys are actually present.
    for (const char* k : {"rep_guard_enabled", "rep_rate_min_hz",
                          "rep_rate_max_hz", "rep_cv_min", "rep_cv_max",
                          "rep_min_events", "rep_broadband_keep_khz",
                          "hop_size_samples"}) {
        double v = -1.0;
        CHECK(det.getTunable(k, &v));
    }

    // Ship defaults: crickets cluster in the middle of the CV(IDI) axis
    // (roughly 0.5-1.3), not below it; the most metronomic patterns in
    // the corpus are bat feeding buzzes and CF species that must be
    // spared. The veto therefore fires on a CV *band*, not a floor.
    // See BandEnergyDetector.hpp for the full rationale.
    //
    // The guard itself ships OFF (0.3.0-rc1): on the Pipistrellus
    // reference corpus, leaving it on discarded ~2500 real bat calls per
    // night that the primary sweep-shape gate had already accepted. The
    // band bounds below stay meaningful because CF-heavy sites turn the
    // guard back on with rep_guard_enabled=1.
    double v = -1.0;
    REQUIRE(det.getTunable("rep_guard_enabled", &v));
    CHECK(v == 0.0);
    REQUIRE(det.getTunable("rep_rate_min_hz", &v));
    CHECK_THAT(static_cast<float>(v), WithinAbs(1.0f, 1e-4f));
    REQUIRE(det.getTunable("rep_rate_max_hz", &v));
    CHECK_THAT(static_cast<float>(v), WithinAbs(20.0f, 1e-4f));
    REQUIRE(det.getTunable("rep_cv_min", &v));
    CHECK_THAT(static_cast<float>(v), WithinAbs(0.50f, 1e-4f));
    REQUIRE(det.getTunable("rep_cv_max", &v));
    CHECK_THAT(static_cast<float>(v), WithinAbs(1.30f, 1e-4f));
    REQUIRE(det.getTunable("rep_min_events", &v));
    CHECK(v == 2.0);
    REQUIRE(det.getTunable("rep_broadband_keep_khz", &v));
    CHECK_THAT(static_cast<float>(v), WithinAbs(10.0f, 1e-4f));
}
