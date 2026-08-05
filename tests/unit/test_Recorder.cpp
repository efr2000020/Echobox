// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Recorder cricketDiscard tests. Standing up the full Recorder + DSP +
/// audio ring is heavy for a unit test, but the discard semantics are
/// simple enough to test via a fake IDetectorStateProvider that lets the
/// test script the "kept-events counter unchanged" scenario deterministically.
///
/// The test scripts the sequence:
///  1. detector reports active=true  → recorder opens a WAV
///  2. detector reports active=false → after silenceMs, recorder closes
///  3. cricketDiscard=true AND batLikeEvents unchanged → discard
///     cricketDiscard=true AND batLikeEvents incremented → keep
///     cricketDiscard=false                            → keep regardless

#include "recorder/PreRollBuffer.hpp"
#include "recorder/Recorder.hpp"
#include "recorder/DetectorStateProvider.hpp"
#include "recorder/Sidecar.hpp"
#include "dsp/ISweepTracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using echobox::recorder::DetectorStateSnapshot;
using echobox::recorder::IDetectorStateProvider;
using echobox::recorder::PreRollBuffer;
using echobox::recorder::Recorder;
using echobox::recorder::RecorderConfig;
using echobox::recorder::SaveRejectedMode;
using echobox::recorder::TunableValue;

namespace fs = std::filesystem;

namespace {

// Test double for IDetectorStateProvider. The recorder polls snapshot()
// off a worker thread; we drive the observed state via atomics so a test
// script can flip active/batLikeEvents without racing the recorder.
class FakeProvider : public IDetectorStateProvider {
public:
    DetectorStateSnapshot snapshot() const override {
        DetectorStateSnapshot s{};
        s.active        = m_active.load();
        s.loHz          = 20000.0f;
        s.hiHz          = 45000.0f;
        s.generation    = m_generation.load();
        s.batLikeEvents = m_batLikeEvents.load();
        return s;
    }
    void set_active(bool a) {
        const bool prev = m_active.exchange(a);
        if (prev && !a) ++m_generation;
    }
    void bump_bat_like() { ++m_batLikeEvents; }

    std::size_t fftSize()  const override { return 4096; }
    std::size_t hopSize()  const override { return 512; }
    float       freqLoHz() const override { return 20000.0f; }
    float       freqHiHz() const override { return 192000.0f; }

    // --- Sidecar-side plumbing for the --save-rejected tests ---
    //
    // stage_rejected_event() queues an EventFeatures blob that
    // drainSidecarPayload() will hand back to the recorder on the next
    // drain. set_min_bandwidth_khz() advertises the "gate threshold" the
    // Boundary mode branch classifies near-misses against. Both are guarded
    // by a mutex because drainSidecarPayload/currentTunables are called on
    // the recorder thread while the test thread stages new fixtures.
    void stage_rejected_event(float bandwidth_khz) {
        std::lock_guard<std::mutex> lk(m_mut);
        EventFeatures e{};
        e.bandwidth_khz = bandwidth_khz;
        e.gate_rejected = true;
        m_staged_events.push_back(e);
    }
    void set_min_bandwidth_khz(double v) {
        std::lock_guard<std::mutex> lk(m_mut);
        m_min_bandwidth_khz = v;
        m_have_min_bandwidth = true;
    }
    std::string algorithmName() const override { return "FakeDetector"; }
    bool drainSidecarPayload(SidecarPayload& out) override {
        std::lock_guard<std::mutex> lk(m_mut);
        out.events.assign(m_staged_events.begin(), m_staged_events.end());
        m_staged_events.clear();
        return true;
    }
    bool currentTunables(std::vector<TunableValue>& out) const override {
        std::lock_guard<std::mutex> lk(m_mut);
        out.clear();
        if (m_have_min_bandwidth) {
            TunableValue t;
            t.key    = "min_bandwidth_khz";
            t.value  = m_min_bandwidth_khz;
            t.is_int = false;
            out.push_back(std::move(t));
        }
        return true;
    }

private:
    std::atomic<bool>          m_active{false};
    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<std::uint64_t> m_batLikeEvents{0};
    mutable std::mutex         m_mut;
    std::vector<EventFeatures> m_staged_events;
    double                     m_min_bandwidth_khz{0.0};
    bool                       m_have_min_bandwidth{false};
};

// Feed some synthetic samples into the pre-roll so the recorder has data
// to consume — otherwise appendLiveAudio short-circuits and closes empty.
void feedPreRoll(PreRollBuffer& pr, std::size_t nSamples) {
    std::vector<std::int16_t> buf(nSamples, 1);
    pr.write(std::span<const std::int16_t>(buf));
}

fs::path makeTempOutputDir(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("echobox-test-" + tag + "-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

int countWavs(const fs::path& dir) {
    int n = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() == ".wav") ++n;
    }
    return n;
}

// Count WAVs under a specific subdir (used to distinguish rejected/ from
// accepted-clip trees rooted at the same output dir).
int countWavsIn(const fs::path& dir) {
    if (!fs::exists(dir)) return 0;
    return countWavs(dir);
}

// Simulate a single cricket-only event: the detector goes active for a
// beat, then silent long enough to satisfy the recorder's silenceMs. No
// bat-like counter bump ⇒ recorder enters the discard branch.
void driveOneRejectedEvent(FakeProvider& provider,
                           std::chrono::milliseconds activeFor,
                           std::chrono::milliseconds silentFor) {
    provider.set_active(true);
    std::this_thread::sleep_for(activeFor);
    provider.set_active(false);
    std::this_thread::sleep_for(silentFor);
}

} // namespace


TEST_CASE("Recorder discards a clip whose window saw no bat-like event",
          "[recorder][cricket-discard]") {
    const auto outputDir = makeTempOutputDir("discard");

    PreRollBuffer pr(48000);           // ~1 s @ 48 kHz — plenty for the pre-roll
    feedPreRoll(pr, 32000);

    FakeProvider provider;

    RecorderConfig cfg;
    cfg.outputDir       = outputDir;
    cfg.sampleRate      = 48000;
    cfg.channels        = 1;
    cfg.preRollMs       = 100;
    cfg.silenceMs       = 200;
    cfg.minLengthMs     = 0;
    cfg.maxLengthMs     = 0;             // uncapped
    cfg.pollIntervalMs  = 5;
    cfg.writeSidecar    = false;
    cfg.cricketDiscard  = true;

    Recorder rec(cfg, pr, provider);
    rec.start();

    // Simulate one hot event: active for 100 ms, then silence for 400 ms
    // so the recorder sees a full silence-timeout close.
    provider.set_active(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    provider.set_active(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    rec.stop();

    // Counter never advanced → cricket-gate discard → no WAV on disk.
    CHECK(countWavs(outputDir) == 0);

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder keeps a clip whose window saw at least one bat-like event",
          "[recorder][cricket-discard]") {
    const auto outputDir = makeTempOutputDir("keep");

    PreRollBuffer pr(48000);
    feedPreRoll(pr, 32000);

    FakeProvider provider;

    RecorderConfig cfg;
    cfg.outputDir       = outputDir;
    cfg.sampleRate      = 48000;
    cfg.channels        = 1;
    cfg.preRollMs       = 100;
    cfg.silenceMs       = 200;
    cfg.minLengthMs     = 0;
    cfg.maxLengthMs     = 0;
    cfg.pollIntervalMs  = 5;
    cfg.writeSidecar    = false;
    cfg.cricketDiscard  = true;

    Recorder rec(cfg, pr, provider);
    rec.start();

    provider.set_active(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Bump the kept-events counter mid-clip — the detector's sweep gate
    // has accepted an event. Recorder should keep the WAV on close.
    provider.bump_bat_like();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    provider.set_active(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    rec.stop();

    CHECK(countWavs(outputDir) == 1);

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder keeps a max-length forced close with the event still active",
          "[recorder][cricket-discard][max-length]") {
    // R2v3 short-clip regression guard: with maxLengthMs low enough that a
    // long-continuous event trips the cap while it's still open, the
    // detector has NOT yet stamped the kept-events counter for that event.
    // A naive discard check would drop the clip; the max-length forced-
    // close guard in Recorder::endRecording() must keep it.
    const auto outputDir = makeTempOutputDir("maxlen-active");

    PreRollBuffer pr(48000);
    feedPreRoll(pr, 4800);              // seed some pre-roll history

    FakeProvider provider;

    RecorderConfig cfg;
    cfg.outputDir       = outputDir;
    cfg.sampleRate      = 48000;
    cfg.channels        = 1;
    cfg.preRollMs       = 20;
    cfg.silenceMs       = 60;
    cfg.minLengthMs     = 0;
    cfg.maxLengthMs     = 120;         // small cap so the cap fires first
    cfg.pollIntervalMs  = 5;
    cfg.writeSidecar    = false;
    cfg.cricketDiscard  = true;

    Recorder rec(cfg, pr, provider);
    rec.start();

    // Pump the pre-roll continuously while the event is "active" so the
    // writer accumulates real frames and can actually trip maxLengthMs
    // (otherwise the recorder just drains the seed and stalls at
    // 100 ms of samples).
    std::atomic<bool> producing{true};
    std::thread producer([&]() {
        while (producing.load(std::memory_order_acquire)) {
            feedPreRoll(pr, 480);       // 10 ms @ 48 kHz per tick
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Drive a continuous "active" event that outlasts maxLengthMs. The
    // counter is NEVER bumped, so a naive check would discard. The guard
    // must keep the clip because the event is still open at the forced
    // close.
    provider.set_active(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    provider.set_active(false);
    producing.store(false, std::memory_order_release);
    producer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    rec.stop();

    CHECK(countWavs(outputDir) >= 1);

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder cricket-discard still works at short silence (50 ms)",
          "[recorder][cricket-discard][short-clip]") {
    // R2v3 short-silence defaults must not weaken the cricket filter: a
    // cricket-only clip (no bat-like events) is still discarded at 50 ms
    // silence exactly as at 2000 ms.
    const auto outputDir = makeTempOutputDir("short-silence-discard");

    PreRollBuffer pr(48000);
    feedPreRoll(pr, 32000);

    FakeProvider provider;

    RecorderConfig cfg;
    cfg.outputDir       = outputDir;
    cfg.sampleRate      = 48000;
    cfg.channels        = 1;
    cfg.preRollMs       = 50;
    cfg.silenceMs       = 50;
    cfg.minLengthMs     = 0;
    cfg.maxLengthMs     = 0;
    cfg.pollIntervalMs  = 5;
    cfg.writeSidecar    = false;
    cfg.cricketDiscard  = true;

    Recorder rec(cfg, pr, provider);
    rec.start();

    provider.set_active(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    provider.set_active(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    rec.stop();

    CHECK(countWavs(outputDir) == 0);

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder --save-rejected off keeps discard behaviour byte-identical",
          "[recorder][save-rejected]") {
    // Sanity: with saveRejected=Off, a cricket-only clip is still deleted and
    // no rejected/ subdir is ever created (byte-identical to today).
    const auto outputDir = makeTempOutputDir("save-rejected-off");

    PreRollBuffer pr(48000);
    feedPreRoll(pr, 32000);

    FakeProvider provider;
    provider.stage_rejected_event(0.5f);   // near-miss; won't matter (Off)
    provider.set_min_bandwidth_khz(0.9);

    RecorderConfig cfg;
    cfg.outputDir      = outputDir;
    cfg.sampleRate     = 48000;
    cfg.channels       = 1;
    cfg.preRollMs      = 100;
    cfg.silenceMs      = 200;
    cfg.minLengthMs    = 0;
    cfg.maxLengthMs    = 0;
    cfg.pollIntervalMs = 5;
    cfg.writeSidecar   = false;
    cfg.cricketDiscard = true;
    cfg.saveRejected   = SaveRejectedMode::Off;

    Recorder rec(cfg, pr, provider);
    rec.start();
    driveOneRejectedEvent(provider,
                          std::chrono::milliseconds(80),
                          std::chrono::milliseconds(400));
    rec.stop();

    CHECK(countWavs(outputDir) == 0);
    CHECK_FALSE(fs::exists(outputDir / "rejected"));

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder --save-rejected all preserves every rejected clip",
          "[recorder][save-rejected]") {
    const auto outputDir = makeTempOutputDir("save-rejected-all");

    PreRollBuffer pr(48000);
    feedPreRoll(pr, 32000);

    FakeProvider provider;
    provider.set_min_bandwidth_khz(0.9);
    // Two rejected clips in a row. Stage each event's features before
    // the discard branch runs so the drain sees them.
    provider.stage_rejected_event(0.5f);

    RecorderConfig cfg;
    cfg.outputDir              = outputDir;
    cfg.sampleRate             = 48000;
    cfg.channels               = 1;
    cfg.preRollMs              = 100;
    cfg.silenceMs              = 200;
    cfg.minLengthMs            = 0;
    cfg.maxLengthMs            = 0;
    cfg.pollIntervalMs         = 5;
    cfg.writeSidecar           = true;    // exercise the sidecar path
    cfg.cricketDiscard         = true;
    cfg.saveRejected           = SaveRejectedMode::All;
    cfg.saveRejectedMaxPerHour = 0;       // unlimited (local-run mode)
    cfg.saveRejectedMinDiskMb  = 0;       // do not gate on disk in the test

    Recorder rec(cfg, pr, provider);
    rec.start();

    driveOneRejectedEvent(provider,
                          std::chrono::milliseconds(80),
                          std::chrono::milliseconds(400));
    provider.stage_rejected_event(0.5f);
    feedPreRoll(pr, 32000);
    driveOneRejectedEvent(provider,
                          std::chrono::milliseconds(80),
                          std::chrono::milliseconds(400));

    rec.stop();

    // Both rejected clips landed under rejected/ (not the accepted tree).
    CHECK(countWavsIn(outputDir / "rejected") == 2);
    // Sidecars written alongside the WAVs, one per clip.
    int json = 0;
    for (const auto& e : fs::recursive_directory_iterator(outputDir / "rejected")) {
        if (e.is_regular_file() && e.path().extension() == ".json") ++json;
    }
    CHECK(json == 2);

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder --save-rejected boundary saves only near-threshold clips",
          "[recorder][save-rejected][boundary]") {
    const auto outputDir = makeTempOutputDir("save-rejected-boundary");

    PreRollBuffer pr(48000);
    feedPreRoll(pr, 32000);

    FakeProvider provider;
    // Gate threshold at 0.9 kHz; boundary margin in Recorder.cpp is 0.30
    // kHz. Bandwidth 0.85 is inside the window; 0.20 is far below.
    provider.set_min_bandwidth_khz(0.9);

    RecorderConfig cfg;
    cfg.outputDir              = outputDir;
    cfg.sampleRate             = 48000;
    cfg.channels               = 1;
    cfg.preRollMs              = 100;
    cfg.silenceMs              = 200;
    cfg.minLengthMs            = 0;
    cfg.maxLengthMs            = 0;
    cfg.pollIntervalMs         = 5;
    cfg.writeSidecar           = false;
    cfg.cricketDiscard         = true;
    cfg.saveRejected           = SaveRejectedMode::Boundary;
    cfg.saveRejectedMaxPerHour = 0;
    cfg.saveRejectedMinDiskMb  = 0;

    Recorder rec(cfg, pr, provider);
    rec.start();

    // Clip 1: near-miss (0.85 kHz within 0.30 of 0.9) → should be saved.
    provider.stage_rejected_event(0.85f);
    driveOneRejectedEvent(provider,
                          std::chrono::milliseconds(80),
                          std::chrono::milliseconds(400));
    // Clip 2: far from threshold (0.20 kHz) → should be dropped.
    provider.stage_rejected_event(0.20f);
    feedPreRoll(pr, 32000);
    driveOneRejectedEvent(provider,
                          std::chrono::milliseconds(80),
                          std::chrono::milliseconds(400));

    rec.stop();

    CHECK(countWavsIn(outputDir / "rejected") == 1);

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder --save-rejected respects max-per-hour cap",
          "[recorder][save-rejected][cap]") {
    const auto outputDir = makeTempOutputDir("save-rejected-cap");

    PreRollBuffer pr(48000);
    feedPreRoll(pr, 96000);

    FakeProvider provider;
    provider.set_min_bandwidth_khz(0.9);

    RecorderConfig cfg;
    cfg.outputDir              = outputDir;
    cfg.sampleRate             = 48000;
    cfg.channels               = 1;
    cfg.preRollMs              = 100;
    cfg.silenceMs              = 200;
    cfg.minLengthMs            = 0;
    cfg.maxLengthMs            = 0;
    cfg.pollIntervalMs         = 5;
    cfg.writeSidecar           = false;
    cfg.cricketDiscard         = true;
    cfg.saveRejected           = SaveRejectedMode::All;
    cfg.saveRejectedMaxPerHour = 1;       // cap is one per hour
    cfg.saveRejectedMinDiskMb  = 0;

    Recorder rec(cfg, pr, provider);
    rec.start();

    for (int i = 0; i < 3; ++i) {
        provider.stage_rejected_event(0.5f);
        feedPreRoll(pr, 32000);
        driveOneRejectedEvent(provider,
                              std::chrono::milliseconds(80),
                              std::chrono::milliseconds(400));
    }

    rec.stop();

    // Cap is 1 → exactly one preserved clip; the other two are aborted.
    CHECK(countWavsIn(outputDir / "rejected") == 1);

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder keeps every clip when cricketDiscard is off",
          "[recorder][cricket-discard]") {
    // Kill-switch behaviour: --cricket-filter off must reproduce R1
    // recording — every leading-edge event produces a saved WAV,
    // regardless of the counter.
    const auto outputDir = makeTempOutputDir("filter-off");

    PreRollBuffer pr(48000);
    feedPreRoll(pr, 32000);

    FakeProvider provider;

    RecorderConfig cfg;
    cfg.outputDir       = outputDir;
    cfg.sampleRate      = 48000;
    cfg.channels        = 1;
    cfg.preRollMs       = 100;
    cfg.silenceMs       = 200;
    cfg.minLengthMs     = 0;
    cfg.maxLengthMs     = 0;
    cfg.pollIntervalMs  = 5;
    cfg.writeSidecar    = false;
    cfg.cricketDiscard  = false;   // <<< kill switch

    Recorder rec(cfg, pr, provider);
    rec.start();

    provider.set_active(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    provider.set_active(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    rec.stop();

    CHECK(countWavs(outputDir) == 1);

    fs::remove_all(outputDir);
}
