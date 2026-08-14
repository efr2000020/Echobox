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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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


// --- Injected-clock tests -------------------------------------------------
//
// These pin the seam added for echobox-replay (src/recorder/RecorderClock.hpp).
// The defect they guard against: the recorder's silenceMs timeout and poll
// cadence were read from steady_clock while maxLengthMs was counted in audio
// frames, so any harness that does not run at exactly 1x real time decouples
// the two. Replay ran 20-30x fast and produced 635 / 650 / 650 clips over
// three runs of the same 60 s input.
//
// The tests below are compiled only where the seam is (see
// src/recorder/CMakeLists.txt); the field build has no RecorderConfig::clock
// member at all.

#ifdef ECHOBOX_RECORDER_CLOCK_INJECTION

namespace {

// Scripted time source implementing the same turn-taking protocol as
// echobox::replay::VirtualClock, kept local so tests/unit does not depend on
// the replay tool being built. The driver (test thread) and the recorder
// thread strictly alternate: tick() blocks until the recorder has finished
// its previous poll, then releases exactly one more.
class ScriptedClock : public echobox::recorder::IRecorderClock {
public:
    std::chrono::steady_clock::time_point now() const override {
        std::lock_guard<std::mutex> lk(m_mut);
        return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(m_ns));
    }
    std::chrono::system_clock::time_point wallNow() const override {
        std::lock_guard<std::mutex> lk(m_mut);
        // Epoch deliberately left at system_clock's zero: a filename built
        // from this can never be confused with one built from "now", which
        // is what the reproducibility check below relies on.
        return std::chrono::system_clock::time_point{} + std::chrono::nanoseconds(m_ns);
    }
    void sleepFor(std::chrono::milliseconds d) override {
        std::unique_lock<std::mutex> lk(m_mut);
        if (m_released) {
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return;
        }
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        if (ns == 0) ns = 1;
        m_wakeNs = m_ns + ns;
        m_parked = true;
        m_cvDriver.notify_all();
        m_cvRec.wait(lk, [this] { return m_released || m_ns >= m_wakeNs; });
        m_parked = false;
    }

    /// Hand the recorder exactly one poll, @p d of virtual time later.
    void tick(std::chrono::milliseconds d) {
        {
            std::unique_lock<std::mutex> lk(m_mut);
            // "Parked for a future time" — not merely "parked" — so we
            // cannot race ahead in the window between notifying and the
            // recorder actually waking.
            m_cvDriver.wait(lk, [this] {
                return m_released || (m_parked && m_wakeNs > m_ns);
            });
            m_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        }
        m_cvRec.notify_all();
    }

    void tickFor(std::chrono::milliseconds total, std::chrono::milliseconds step) {
        for (auto t = std::chrono::milliseconds(0); t < total; t += step) tick(step);
    }

    /// Unblock permanently so Recorder::stop() can join its thread.
    void release() {
        {
            std::lock_guard<std::mutex> lk(m_mut);
            m_released = true;
        }
        m_cvRec.notify_all();
        m_cvDriver.notify_all();
    }

private:
    mutable std::mutex      m_mut;
    std::condition_variable m_cvRec;
    std::condition_variable m_cvDriver;
    std::int64_t m_ns{0};
    std::int64_t m_wakeNs{0};
    bool m_parked{false};
    bool m_released{false};
};

// Finalized clips only. countWavs() also matches the in-progress
// "<stem>.partial.wav" temp file, which is exactly what a mid-recording
// assertion must not count.
int countFinalWavs(const fs::path& dir) {
    int n = 0;
    if (!fs::exists(dir)) return 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const auto name = e.path().filename().string();
        if (e.path().extension() != ".wav") continue;
        if (name.find(".partial.") != std::string::npos) continue;
        ++n;
    }
    return n;
}

// One scripted event under an injected clock: active for `activeFor` of
// virtual time, then quiet for `silentFor`. Returns the number of WAVs left
// on disk. Nothing here sleeps on wall time.
int runScriptedEvent(const fs::path& outputDir,
                     std::chrono::milliseconds activeFor,
                     std::chrono::milliseconds silentFor,
                     std::uint32_t silenceMs) {
    PreRollBuffer pr(48000);
    feedPreRoll(pr, 32000);

    FakeProvider provider;
    ScriptedClock clock;

    RecorderConfig cfg;
    cfg.outputDir      = outputDir;
    cfg.sampleRate     = 48000;
    cfg.channels       = 1;
    cfg.preRollMs      = 100;
    cfg.silenceMs      = silenceMs;
    cfg.minLengthMs    = 0;
    cfg.maxLengthMs    = 0;          // uncapped: isolate the silence timeout
    cfg.pollIntervalMs = 1;
    cfg.writeSidecar   = false;
    cfg.cricketDiscard = false;
    cfg.clock          = &clock;

    Recorder rec(cfg, pr, provider);
    rec.start();

    constexpr auto kStep = std::chrono::milliseconds(10);
    provider.set_active(true);
    clock.tickFor(activeFor, kStep);
    provider.set_active(false);
    clock.tickFor(silentFor, kStep);

    clock.release();
    rec.stop();
    return countWavs(outputDir);
}

} // namespace


TEST_CASE("Recorder silence timeout runs on injected time, not wall time",
          "[recorder][clock]") {
    // A 3 s silence window that the recorder must measure in *its* clock.
    // If it still read steady_clock this test would need 3 s of real time
    // to close the WAV; driven virtually it closes in milliseconds, and the
    // wall-clock assertion below is what proves the difference.
    const auto outputDir = makeTempOutputDir("clock-virtual");
    const auto t0 = std::chrono::steady_clock::now();

    const int wavs = runScriptedEvent(outputDir,
                                      std::chrono::milliseconds(100),
                                      std::chrono::milliseconds(3200),
                                      /*silenceMs=*/3000);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    CHECK(wavs == 1);
    // 3.3 s of virtual time in well under a second of real time. Generous
    // bound: the point is the order of magnitude, not a latency budget.
    CHECK(elapsed < std::chrono::milliseconds(1500));

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder does not close before the injected silence window elapses",
          "[recorder][clock]") {
    // The other half of the contract: virtual time must be *required*, not
    // merely sufficient. Stopping 500 ms of virtual time short of the
    // window leaves the recording open, so the only WAV on disk is the one
    // the shutdown flush writes — never a silence-timeout close.
    const auto outputDir = makeTempOutputDir("clock-early");

    PreRollBuffer pr(48000);
    feedPreRoll(pr, 32000);

    FakeProvider provider;
    ScriptedClock clock;

    RecorderConfig cfg;
    cfg.outputDir      = outputDir;
    cfg.sampleRate     = 48000;
    cfg.channels       = 1;
    cfg.preRollMs      = 100;
    cfg.silenceMs      = 1000;
    cfg.minLengthMs    = 0;
    cfg.maxLengthMs    = 0;
    cfg.pollIntervalMs = 1;
    cfg.writeSidecar   = false;
    cfg.cricketDiscard = false;
    cfg.clock          = &clock;

    Recorder rec(cfg, pr, provider);
    rec.start();

    provider.set_active(true);
    clock.tickFor(std::chrono::milliseconds(100), std::chrono::milliseconds(10));
    provider.set_active(false);
    clock.tickFor(std::chrono::milliseconds(500), std::chrono::milliseconds(10));

    // 500 ms of virtual quiet against a 1000 ms window: still open, so
    // only the .partial temp file exists — no finalized clip.
    CHECK(countFinalWavs(outputDir) == 0);

    clock.release();
    rec.stop();

    fs::remove_all(outputDir);
}


TEST_CASE("Recorder output is reproducible under a scripted clock",
          "[recorder][clock][determinism]") {
    // The acceptance criterion the replay harness needs: identical input +
    // identical scripted time ⇒ identical files, by name and by size. Under
    // the old wall-clock recorder the same corpus produced a different clip
    // count on every run.
    std::vector<std::vector<std::pair<std::string, std::uintmax_t>>> runs;
    std::vector<fs::path> dirs;

    for (int i = 0; i < 3; ++i) {
        const auto dir = makeTempOutputDir("clock-repeat-" + std::to_string(i));
        dirs.push_back(dir);
        CHECK(runScriptedEvent(dir,
                               std::chrono::milliseconds(100),
                               std::chrono::milliseconds(400),
                               /*silenceMs=*/200) == 1);

        std::vector<std::pair<std::string, std::uintmax_t>> files;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            // Record the path relative to the run dir (which carries a
            // unique tag) plus the size, so the comparison covers the
            // wall-clock-derived filename as well as the audio length.
            files.emplace_back(fs::relative(e.path(), dir).string(),
                               e.file_size());
        }
        std::sort(files.begin(), files.end());
        runs.push_back(std::move(files));
    }

    CHECK(runs[0] == runs[1]);
    CHECK(runs[0] == runs[2]);
    REQUIRE(runs[0].size() == 1);

    // The filename must come from the injected wall clock, not the host's.
    // The scripted epoch is system_clock's zero, so the date directory is
    // 1969-12-31 or 1970-01-01 depending on the host time zone — either
    // way it cannot be today. If wallNow() injection regresses, this fails.
    const auto dateDir = fs::path(runs[0][0].first).parent_path().string();
    CHECK(dateDir.rfind("19", 0) == 0);

    for (const auto& d : dirs) fs::remove_all(d);
}

#endif // ECHOBOX_RECORDER_CLOCK_INJECTION
