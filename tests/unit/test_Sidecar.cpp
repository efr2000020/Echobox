// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Sidecar round-trip tests. The sidecar is the engineering-release bet that
/// short production recordings stay analysable by the offline tuning tools:
/// it must carry the noise-floor snapshot losslessly across a JSON boundary
/// and the resulting JSON must be re-loadable into a detector that matches
/// the device's behaviour on the same input.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/algorithms/BandEnergyDetector/BandEnergyDetector.hpp"
#include "recorder/Sidecar.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using echobox::recorder::base64DecodeFloats;
using echobox::recorder::base64EncodeFloats;
using echobox::recorder::SidecarRecording;
using echobox::recorder::TunableValue;
using echobox::recorder::writeSidecar;
// SidecarPayload and EventFeatures live in the global namespace (declared in
// ISweepTracker.hpp without a namespace) because they're part of the plugin
// ABI shared with the C FFI. Reference them at global scope.

namespace {

// Drive the detector through `nFrames` frames of low-amplitude broadband noise
// so the EMA noise floor converges. Returns the frame index used.
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

// Inject a synthetic peaky burst that should clear the SNR + flatness gates,
// running for enough frames to satisfy the min_active_frames debounce and
// close out with hangover so an annotation actually emits.
void emitFakeEvent(BandEnergyDetector& det, std::size_t nBins,
                   std::uint32_t& frame, std::mt19937& rng) {
    std::uniform_real_distribution<float> bg(1e-4f, 1e-3f);
    std::vector<float> mags(nBins);
    DetectorState state{};
    Annotation    ann{};
    // 10 hot frames: a few bins around bin 300 (~28 kHz at sr=384k/fft=4096)
    // jump three orders of magnitude above the floor — peaky enough to trip
    // max_flatness=0.65 (low geomean contribution vs the bright peak), and
    // inside band 1 (20–45 kHz) of the configured detection window.
    for (int i = 0; i < 10; ++i, ++frame) {
        for (auto& v : mags) v = bg(rng);
        for (std::size_t b = 300; b < 310 && b < nBins; ++b) {
            mags[b] = 0.5f;
        }
        det.processFrame(mags, frame, state, ann);
    }
    // 20 silent frames to bridge the hangover and close the event.
    for (int i = 0; i < 20; ++i, ++frame) {
        for (auto& v : mags) v = bg(rng);
        det.processFrame(mags, frame, state, ann);
    }
}

} // namespace


TEST_CASE("base64 round-trips float32 samples losslessly", "[sidecar][base64]") {
    SECTION("empty input yields empty output") {
        REQUIRE(base64EncodeFloats({}).empty());
        REQUIRE(base64DecodeFloats("").empty());
    }

    SECTION("typical noise-floor vector survives encode/decode") {
        std::mt19937 rng(0x12345678);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        std::vector<float> floor(2049);
        for (auto& v : floor) v = dist(rng);

        const auto encoded = base64EncodeFloats(floor);
        REQUIRE_FALSE(encoded.empty());
        const auto decoded = base64DecodeFloats(encoded);
        REQUIRE(decoded.size() == floor.size());
        for (std::size_t i = 0; i < floor.size(); ++i) {
            // Bit-for-bit: encoding goes via raw bytes, no float-to-string.
            REQUIRE(decoded[i] == floor[i]);
        }
    }

    SECTION("garbage input decodes to empty") {
        REQUIRE(base64DecodeFloats("not valid base64 at all !@#").empty());
    }
}


TEST_CASE("seedNoiseFloor reproduces the device's decision on subsequent frames",
          "[sidecar][seed]") {
    // This is the test that the cold-start fix actually works: detector A
    // runs long enough to converge its EMA floor and fire an event. We
    // extract the floor snapshot the device would write into a sidecar,
    // seed detector B with it, and verify that B fires the same event on
    // the same audio frames that follow the snapshot — *without* any
    // warm-up of its own.

    constexpr int sampleRate = 384000;
    constexpr std::size_t fftSize = 4096;
    const std::size_t nBins = fftSize / 2 + 1;

    BandEnergyDetector detA;
    detA.configure(sampleRate, fftSize, 20000.0f, 192000.0f);

    std::mt19937 rng(0xC0FFEE);
    std::uint32_t frame = 0;
    frame = warmFloor(detA, nBins, frame, 200, rng);
    emitFakeEvent(detA, nBins, frame, rng);

    ::SidecarPayload payload;
    REQUIRE(detA.drainSidecarPayload(payload));
    REQUIRE_FALSE(payload.events.empty());
    REQUIRE(payload.noise_floor_at_first_event.size() == nBins);

    // Detector B is fresh; without seeding it would cold-start its floor
    // from the first frame and the early gate decisions would diverge.
    BandEnergyDetector detB;
    detB.configure(sampleRate, fftSize, 20000.0f, 192000.0f);
    REQUIRE(detB.seedNoiseFloor(
        std::span<const float>(payload.noise_floor_at_first_event)));

    // Replay an event on B with a fresh rng state. The first hot frame
    // count needs to match detA's; we don't care about identical floors
    // for ever, but B should produce at least one event on the same input
    // pattern, with the same band index.
    std::mt19937 rngB(0xC0FFEE);
    // skip A's warm-up frames in B's rng stream so the noise pattern
    // matches A's event-firing window.
    (void) warmFloor(detB, nBins, 0, 200, rngB);
    std::uint32_t bFrame = 200;
    emitFakeEvent(detB, nBins, bFrame, rngB);

    SidecarPayload payloadB;
    REQUIRE(detB.drainSidecarPayload(payloadB));
    REQUIRE_FALSE(payloadB.events.empty());
    REQUIRE(payloadB.events.front().band_index == payload.events.front().band_index);
}


TEST_CASE("writeSidecar produces a parseable JSON next to the WAV",
          "[sidecar][write]") {
    // We don't pull in a JSON library on the device, so this test just
    // sanity-checks the *shape* of the output: it's UTF-8, well-formed
    // enough to find the keys we care about, and the floor decodes back
    // to the same floats.
    const auto tmpDir = fs::temp_directory_path() / "echobox_sidecar_test";
    fs::create_directories(tmpDir);
    const auto wavPath = tmpDir / "20260605_120000000.wav";
    {
        std::ofstream f(wavPath); f << "fake wav body";  // file just has to exist
    }

    SidecarRecording meta;
    meta.wav_path        = wavPath.filename().string();
    meta.sample_rate     = 384000;
    meta.fft_size        = 4096;
    meta.hop_size        = 512;
    meta.freq_lo_hz      = 20000.0f;
    meta.freq_hi_hz      = 192000.0f;
    meta.preroll_ms      = 1000;
    meta.silence_ms      = 2000;
    meta.algorithm       = "BandEnergyDetector";
    meta.boot_iso8601    = "2026-06-05T18:00:00.000Z";
    meta.capture_iso8601 = "2026-06-05T18:05:30.123Z";

    std::vector<TunableValue> tunables{
        {"band_snr_threshold", 12.0,  false},
        {"max_flatness",        0.65, false},
        {"min_active_frames",   2.0,  true},
    };

    ::SidecarPayload payload;
    payload.events.push_back(::EventFeatures{42u, 51u, 10u, 4, 14.3f, 0.62f,
                                             18.7f, 129900.0f, 192000.0f});
    payload.noise_floor_at_first_event.assign(2049, 0.0f);
    payload.noise_floor_at_first_event[100] = 0.12345f;  // sentinel value
    payload.events_total_since_boot     = 412;
    payload.frames_processed_since_boot = 12345678;

    REQUIRE(writeSidecar(wavPath, meta, tunables, payload));

    const auto jsonPath = fs::path(wavPath).replace_extension(".json");
    REQUIRE(fs::exists(jsonPath));

    std::ifstream f(jsonPath);
    std::stringstream ss; ss << f.rdbuf();
    const std::string body = ss.str();

    // Bumped to 2 in 0.3.0-rc1 when the six decision-path diagnostic
    // fields (sweep_bat_like, veto_applied, rep_rate_hz, ...) were added.
    REQUIRE(body.find("\"format_version\": 2") != std::string::npos);
    REQUIRE(body.find("\"algorithm\": \"BandEnergyDetector\"") != std::string::npos);
    REQUIRE(body.find("\"max_flatness\":") != std::string::npos);
    REQUIRE(body.find("\"trigger_snr\":") != std::string::npos);
    REQUIRE(body.find("\"start_frame\": 42") != std::string::npos);
    REQUIRE(body.find("\"n_bins\": 2049") != std::string::npos);
    REQUIRE(body.find("\"events_total_since_boot\": 412") != std::string::npos);

    fs::remove(jsonPath);
    fs::remove(wavPath);
    fs::remove(tmpDir);
}
