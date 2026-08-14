// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Sweep-shape feature + cricket/noise-gate tests.
///
/// Two halves, because the two things are no longer the same thing:
///
///  - The pure helper BandEnergyDetector::computeSweepShape() is tested
///    directly (smooth FM sweep vs. harmonic hopping vs. flat narrowband
///    vs. flat broadband). These four features are now DIAGNOSTICS — they
///    are still emitted into the sidecar but no longer decide anything —
///    so the cases below pin the arithmetic, not a verdict.
///  - The confident-reject gate is driven through processFrame(): its
///    conjunction, each threshold as a live tunable, the single-verdict
///    property (no fast-drop on outState.active), the batLike counter
///    contract the recorder depends on, and manifest lock-step.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/algorithms/BandEnergyDetector/BandEnergyDetector.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <algorithm>
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


// --- Detector-level integration: the confident-reject noise gate ---------
//
// Fixtures below use a *constant* background rather than random noise, so
// the noise-floor EMA converges to it exactly and every SNR quoted here is
// arithmetic, not approximate. A flat background also has spectral
// flatness 1.0, which is above max_flatness (0.80), so background frames
// are never hot and events start and stop exactly where the fixture says.

namespace {

constexpr int         kSampleRate  = 384000;
constexpr std::size_t kFftSize     = 4096;
constexpr std::size_t kNumBins     = kFftSize / 2 + 1;   // 2049
constexpr float       kBinHz       = 93.75f;             // 384000 / 4096

// Sub-band start bins, from BandEnergyDetector::BAND_EDGES_HZ at kBinHz:
// band 1 = 20-45 kHz (bin 213), band 2 = 45-80 kHz (bin 480).
constexpr std::size_t kBand1Start  = 213;
constexpr std::size_t kBand2Start  = 480;

// Flat background. The floor converges here, and flatness(constant) = 1.0
// > max_flatness, so these frames are never hot.
constexpr float       kQuietMag    = 1e-3f;

// A "weak broadband" trigger: a plateau of kPlateauBins bins at 11x the
// converged floor. That lands the event squarely in the reject corner —
//   trigger SNR  ~10.5  (< noise_snr_max 12; the floor has crept up one
//                        frame by the time min_active_frames opens it)
//   flatness     ~0.63  (> noise_flatness_min 0.55, < max_flatness 0.80:
//                        185 raised bins out of 1835 in-band bins)
//   band index    1     (<= noise_band_max)
// Deliberately close to the thresholds on every axis, so a test that moves
// one threshold by a little moves the verdict.
constexpr float       kWeakMag     = 1.1e-2f;
constexpr std::size_t kPlateauBins = 185;

struct EventOutcome {
    int           activeFrames = 0;   // frames outState.active was reported
    int           annotations  = 0;   // closed-event annotations emitted
    std::uint64_t batLike      = 0;   // batLikeEventsSinceBoot delta
    std::uint64_t rejected     = 0;   // rejectedEventsSinceBoot delta
    bool          haveEvent    = false;
    EventFeatures event{};            // the single closed event's sidecar record
};

void warmFloor(BandEnergyDetector& det, std::uint32_t& frame, int nFrames) {
    std::vector<float> mags(kNumBins, kQuietMag);
    DetectorState state{};
    Annotation    ann{};
    for (int i = 0; i < nFrames; ++i, ++frame) {
        det.processFrame(mags, frame, state, ann);
    }
}

// Drive one event: `nBins` bins from `startBin` raised to `mag` for 8
// frames (after which the rising floor pulls band SNR under the 8.0
// threshold on its own), then 20 quiet frames to run the hangover out and
// close the event. Returns everything an observer downstream of the
// detector could see.
EventOutcome runPlateauEvent(BandEnergyDetector& det, std::uint32_t& frame,
                             std::size_t startBin,
                             std::size_t nBins = kPlateauBins,
                             float       mag   = kWeakMag) {
    EventOutcome out;
    const std::uint64_t batLike0  = det.batLikeEventsSinceBoot();
    const std::uint64_t rejected0 = det.rejectedEventsSinceBoot();

    std::vector<float> mags(kNumBins, kQuietMag);
    DetectorState state{};
    Annotation    ann{};

    for (int i = 0; i < 8; ++i, ++frame) {
        std::fill(mags.begin(), mags.end(), kQuietMag);
        for (std::size_t b = startBin; b < startBin + nBins && b < kNumBins; ++b) {
            mags[b] = mag;
        }
        if (det.processFrame(mags, frame, state, ann)) ++out.annotations;
        if (state.active) ++out.activeFrames;
    }
    std::fill(mags.begin(), mags.end(), kQuietMag);
    for (int i = 0; i < 20; ++i, ++frame) {
        if (det.processFrame(mags, frame, state, ann)) ++out.annotations;
        if (state.active) ++out.activeFrames;
    }

    out.batLike  = det.batLikeEventsSinceBoot()  - batLike0;
    out.rejected = det.rejectedEventsSinceBoot() - rejected0;

    ::SidecarPayload payload;
    det.drainSidecarPayload(payload);
    if (!payload.events.empty()) {
        out.haveEvent = true;
        out.event     = payload.events.front();
    }
    return out;
}

// BandEnergyDetector holds atomics + a mutex, so it is neither copyable
// nor movable; configure in place rather than returning one by value.
void makeDetector(BandEnergyDetector& det) {
    det.configure(kSampleRate, kFftSize, 20000.0f, 192000.0f);
}

} // namespace


TEST_CASE("noise gate: shipped defaults are the measured operating point",
          "[noise][gate][tunables]") {
    // The rule and its three thresholds are the §7b.6 recommended row
    // (per-call recall 96.04 % / 97.66 % on the two field nights, removing
    // 41.4 % / 25.6 % of non-bat clips). If a future round retunes these,
    // it should be a deliberate edit here too.
    BandEnergyDetector det;
    makeDetector(det);
    double v = -1.0;
    REQUIRE(det.getTunable("noise_reject_enabled", &v));
    CHECK(v == 1.0);
    REQUIRE(det.getTunable("noise_snr_max", &v));
    CHECK(v == 12.0);
    REQUIRE(det.getTunable("noise_flatness_min", &v));
    CHECK_THAT(static_cast<float>(v), WithinAbs(0.55f, 1e-6f));
    REQUIRE(det.getTunable("noise_band_max", &v));
    CHECK(v == 1.0);
}


TEST_CASE("noise gate: a weak broadband low-band trigger is rejected",
          "[noise][gate][detector]") {
    BandEnergyDetector det;
    makeDetector(det);
    std::uint32_t frame = 0;
    warmFloor(det, frame, 60);

    const EventOutcome ev = runPlateauEvent(det, frame, kBand1Start);

    REQUIRE(ev.haveEvent);
    CHECK(ev.event.band_index == 1);
    CHECK(ev.event.trigger_snr  <  12.0f);
    CHECK(ev.event.trigger_flatness > 0.55f);
    CHECK(ev.event.gate_rejected == true);

    // The counter contract the recorder's endRecording discard depends on:
    // exactly one counter moves per closed event, and for a rejected event
    // it must be the rejected one — a clip containing only this event sees
    // batLikeEvents unchanged and is dropped.
    CHECK(ev.batLike  == 0);
    CHECK(ev.rejected == 1);
    // No closed-event annotation for a rejected event, so an annotation-
    // counting consumer sees gate-on and gate-off differ.
    CHECK(ev.annotations == 0);
}


TEST_CASE("noise gate: rejection is a conjunction — any bat-like axis keeps",
          "[noise][gate][detector]") {
    // The whole point of the replacement is directional: reject only what
    // can be confirmed non-bat. Each section below leaves the fixture event
    // exactly as-is and moves ONE threshold past it, which is the same as
    // the event failing that one condition. All three must keep.
    std::uint32_t frame = 0;

    SECTION("strong enough trigger SNR") {
        BandEnergyDetector det;
        makeDetector(det);
        REQUIRE(det.setTunable("noise_snr_max", 8.0));  // event's ~10.5 is above
        warmFloor(det, frame, 60);
        const EventOutcome ev = runPlateauEvent(det, frame, kBand1Start);
        REQUIRE(ev.haveEvent);
        CHECK(ev.event.gate_rejected == false);
        CHECK(ev.batLike == 1);
        CHECK(ev.annotations == 1);
    }

    SECTION("not broadband enough") {
        BandEnergyDetector det;
        makeDetector(det);
        REQUIRE(det.setTunable("noise_flatness_min", 0.70));  // event's ~0.63 is below
        warmFloor(det, frame, 60);
        const EventOutcome ev = runPlateauEvent(det, frame, kBand1Start);
        REQUIRE(ev.haveEvent);
        CHECK(ev.event.gate_rejected == false);
        CHECK(ev.batLike == 1);
    }

    SECTION("above the noise band, by tunable") {
        BandEnergyDetector det;
        makeDetector(det);
        REQUIRE(det.setTunable("noise_band_max", 0.0));  // event's band 1 is above
        warmFloor(det, frame, 60);
        const EventOutcome ev = runPlateauEvent(det, frame, kBand1Start);
        REQUIRE(ev.haveEvent);
        CHECK(ev.event.gate_rejected == false);
        CHECK(ev.batLike == 1);
    }

    SECTION("above the noise band, by triggering in sub-band 2") {
        // Same weak broadband plateau, moved into 45-80 kHz. Nothing about
        // the event changes except where it sits, and that alone keeps it —
        // this is the AUC-0.748 band feature doing the work.
        BandEnergyDetector det;
        makeDetector(det);
        warmFloor(det, frame, 60);
        const EventOutcome ev = runPlateauEvent(det, frame, kBand2Start);
        REQUIRE(ev.haveEvent);
        CHECK(ev.event.band_index == 2);
        CHECK(ev.event.trigger_snr < 12.0f);
        CHECK(ev.event.trigger_flatness > 0.55f);
        CHECK(ev.event.gate_rejected == false);
        CHECK(ev.batLike == 1);
    }
}


TEST_CASE("noise gate: the master switch restores pre-gate behaviour",
          "[noise][gate][detector]") {
    // --cricket-filter off must be a true no-op on the detector side: same
    // event, kept, annotated, counted bat-like.
    BandEnergyDetector det;
    makeDetector(det);
    REQUIRE(det.setTunable("noise_reject_enabled", 0.0));
    std::uint32_t frame = 0;
    warmFloor(det, frame, 60);

    const EventOutcome ev = runPlateauEvent(det, frame, kBand1Start);
    REQUIRE(ev.haveEvent);
    CHECK(ev.event.gate_rejected == false);
    CHECK(ev.batLike  == 1);
    CHECK(ev.rejected == 0);
    CHECK(ev.annotations == 1);
}


TEST_CASE("noise gate: one verdict — outState.active is never suppressed",
          "[noise][gate][detector][regression]") {
    // The retired sweep gate ran a provisional verdict on partial event
    // data and dropped outState.active mid-event. That fast-drop machinery
    // is the source of correctness bugs A2/A3/A7 (a suppressed cricket
    // event swallowing a bat call that arrived inside its hangover), and it
    // is deliberately gone: the filter's only channel to the recorder is
    // the batLike counter at event close. So a rejected event must report
    // exactly as many active frames as the same event with the gate off.
    std::uint32_t frameOn = 0, frameOff = 0;

    BandEnergyDetector on;
    makeDetector(on);
    warmFloor(on, frameOn, 60);
    const EventOutcome evOn = runPlateauEvent(on, frameOn, kBand1Start);

    BandEnergyDetector off;
    makeDetector(off);
    REQUIRE(off.setTunable("noise_reject_enabled", 0.0));
    warmFloor(off, frameOff, 60);
    const EventOutcome evOff = runPlateauEvent(off, frameOff, kBand1Start);

    REQUIRE(evOn.event.gate_rejected  == true);
    REQUIRE(evOff.event.gate_rejected == false);
    CHECK(evOn.activeFrames > 0);
    CHECK(evOn.activeFrames == evOff.activeFrames);
    // Event geometry is identical too — the verdict changes what the
    // recorder does with the clip, not what the detector reports.
    CHECK(evOn.event.start_frame - 60u == evOff.event.start_frame - 60u);
    CHECK(evOn.event.duration_frames == evOff.event.duration_frames);
}


TEST_CASE("noise gate: sweep-shape diagnostics survive the verdict retirement",
          "[noise][gate][detector][diagnostics]") {
    // The four sweep features no longer decide anything, but offline
    // tooling reads them out of the sidecar and the next gate iteration
    // will be designed against them, so they must still be populated on a
    // shipped capture. The fixture event makes the point sharply: its
    // 10-dB bandwidth is ~17 kHz, so the RETIRED sweep verdict would have
    // kept it (sweep_bat_like == true) while the new rule rejects it.
    BandEnergyDetector det;
    makeDetector(det);
    std::uint32_t frame = 0;
    warmFloor(det, frame, 60);

    const EventOutcome ev = runPlateauEvent(det, frame, kBand1Start);
    REQUIRE(ev.haveEvent);
    CHECK(ev.event.gate_rejected  == true);
    CHECK(ev.event.sweep_bat_like == true);
    CHECK(ev.event.bandwidth_khz  > 1.0f);
    CHECK_THAT(ev.event.mono_fraction, WithinAbs(1.0f, 1e-6f));
    // Retired with the two-tier split; kept in the schema, always false.
    CHECK(ev.event.provisional_rejected == false);
    // Rejected events still reach the sidecar so the validator can A/B
    // without a rebuild.
    CHECK(ev.event.duration_frames > 0);
}


TEST_CASE("tunable manifest lists exactly the keys set/getTunable accept",
          "[noise][tunables][lockstep]") {
    // Drift between listTunables(), setTunable() and getTunable() silently
    // hides a knob from the GUI/validator. Enumerate the manifest and verify
    // each key round-trips via set+get.
    BandEnergyDetector det;
    makeDetector(det);
    for (const auto& t : det.listTunables()) {
        double v = -1.0;
        REQUIRE(det.getTunable(t.key, &v));
        REQUIRE(det.setTunable(t.key, t.default_value));
    }
    // Spot-check the four confident-reject keys, plus the sweep keys that
    // survive as diagnostics / rep-guard bypass parameters.
    for (const char* k : {"noise_reject_enabled", "noise_snr_max",
                          "noise_flatness_min", "noise_band_max",
                          "min_bandwidth_khz", "sweep_drift_khz",
                          "sweep_path_ratio_max", "sweep_mono_frac_min"}) {
        double v = -1.0;
        CHECK(det.getTunable(k, &v));
    }
    // The retired master switch is gone, not silently accepted-and-ignored.
    double dead = -1.0;
    CHECK_FALSE(det.getTunable("sweep_gate_enabled", &dead));
    CHECK_FALSE(det.setTunable("sweep_gate_enabled", 1.0));
}
