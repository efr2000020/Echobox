// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// Unit tests for the collection Session: header/end-marker lifecycle,
/// governor trip on max-duration, storage-math helper. Kept off any
/// hardware or ALSA — these run happily in CI.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "collection/CollectionConfig.hpp"
#include "collection/SampleClock.hpp"
#include "collection/Session.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using echobox::collection::CollectionConfig;
using echobox::collection::SampleClock;
using echobox::collection::Session;
using echobox::collection::SessionMetadata;

namespace {

std::filesystem::path uniqueTmpDir(const char* tag) {
    const auto base = std::filesystem::temp_directory_path();
    const auto ns   = std::chrono::steady_clock::now().time_since_epoch().count();
    auto d = base / (std::string("echobox_session_test_") + tag + "_" + std::to_string(ns));
    std::filesystem::create_directories(d);
    return d;
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

SessionMetadata makeMeta() {
    SessionMetadata m;
    m.firmwareSha    = "deadbeef";
    m.algorithm      = "BandEnergyDetector";
    m.sampleRate     = 384000;
    m.channels       = 1;
    m.micDevice      = "plughw:CARD=UltraMic384K";
    m.siteNote       = "unit-test site";
    m.configJsonBlob = R"({"preroll_ms":50,"silence_ms":50})";
    return m;
}

} // namespace


TEST_CASE("estimateMaxHours: 384kHz mono int16 storage math", "[collection]") {
    // 384 kHz * 2 B/frame * 3600 s/h = 2 764 800 000 B/h = 2637.7 MiB/h.
    // Round-trip: for exactly one hour of audio we'd need 2637.7 MiB free.
    const double h = echobox::collection::estimateMaxHours(2637, 384000);
    REQUIRE_THAT(h, Catch::Matchers::WithinAbs(1.0, 0.01));

    // Double the space → double the hours (linearity check).
    const double h2 = echobox::collection::estimateMaxHours(5274, 384000);
    REQUIRE_THAT(h2 / h, Catch::Matchers::WithinAbs(2.0, 0.01));

    // Halving the sample rate doubles the hours (rate-independence check).
    const double hHalfRate = echobox::collection::estimateMaxHours(2637, 192000);
    REQUIRE_THAT(hHalfRate / h, Catch::Matchers::WithinAbs(2.0, 0.01));

    // Zero-safe: no free space, no sample rate.
    REQUIRE(echobox::collection::estimateMaxHours(0, 384000) == 0.0);
    REQUIRE(echobox::collection::estimateMaxHours(1000, 0)   == 0.0);
}


TEST_CASE("Session::start() is a no-op when disabled", "[collection]") {
    const auto dir = uniqueTmpDir("disabled");
    SampleClock clock;
    CollectionConfig cfg;
    cfg.enabled = false;

    Session s(cfg, clock, dir);
    REQUIRE(s.start(makeMeta()));   // returns true (no-op success)
    // Header file must NOT exist — the overlay is off.
    REQUIRE_FALSE(std::filesystem::exists(s.headerPath()));

    s.stop();
    REQUIRE_FALSE(std::filesystem::exists(s.endMarkerPath()));

    std::filesystem::remove_all(dir);
}


TEST_CASE("Session writes header and end marker across a clean lifecycle",
          "[collection]") {
    const auto dir = uniqueTmpDir("lifecycle");
    SampleClock clock;

    CollectionConfig cfg;
    cfg.enabled = true;
    cfg.dir     = dir;
    cfg.governor.maxDurationSec  = 0;   // uncapped for this test
    cfg.governor.minFreeMbFloor  = 0;   // no floor
    cfg.governor.pollIntervalSec = 1;   // fast poll so join is quick

    Session s(cfg, clock, dir);
    REQUIRE(s.start(makeMeta()));
    REQUIRE(std::filesystem::exists(s.headerPath()));

    // Simulate some frames captured.
    clock.advance(1000);
    clock.advance(2000);

    s.stop();
    REQUIRE(std::filesystem::exists(s.endMarkerPath()));

    const auto header = readAll(s.headerPath());
    REQUIRE(header.find("\"firmware_sha\":    \"deadbeef\"") != std::string::npos);
    REQUIRE(header.find("\"algorithm\":       \"BandEnergyDetector\"") != std::string::npos);
    REQUIRE(header.find("\"sample_rate\":     384000")   != std::string::npos);
    REQUIRE(header.find("\"start_sample\":    0")        != std::string::npos);

    const auto endMarker = readAll(s.endMarkerPath());
    REQUIRE(endMarker.find("\"end_sample\":       3000")     != std::string::npos);
    REQUIRE(endMarker.find("\"samples_captured\": 3000")     != std::string::npos);
    // stopReason() is empty when we ended via stop() rather than governor
    // trip → the marker records "shutdown-signal".
    REQUIRE(endMarker.find("\"reason\":           \"shutdown-signal\"")
            != std::string::npos);

    std::filesystem::remove_all(dir);
}


TEST_CASE("Session governor trips on max-duration and records the reason",
          "[collection]") {
    const auto dir = uniqueTmpDir("governor");
    SampleClock clock;

    CollectionConfig cfg;
    cfg.enabled = true;
    cfg.dir     = dir;
    cfg.governor.maxDurationSec  = 1;   // trip after 1 second wall-clock
    cfg.governor.minFreeMbFloor  = 0;
    cfg.governor.pollIntervalSec = 1;

    Session s(cfg, clock, dir);
    REQUIRE(s.start(makeMeta()));

    // Wait for the governor to trip (poll interval + a little slack).
    for (int i = 0; i < 40; ++i) {   // up to 4 s
        if (s.stopRequested()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(s.stopRequested());
    REQUIRE(s.stopReason().find("max-duration-reached") != std::string::npos);

    s.stop();
    const auto endMarker = readAll(s.endMarkerPath());
    REQUIRE(endMarker.find("max-duration-reached") != std::string::npos);

    std::filesystem::remove_all(dir);
}


TEST_CASE("Session::start() writes a header that includes the streams block",
          "[collection]") {
    const auto dir = uniqueTmpDir("streams");
    SampleClock clock;
    CollectionConfig cfg;
    cfg.enabled = true;
    cfg.dir     = dir;
    cfg.streams.streamA = true;
    cfg.streams.streamB = false;
    cfg.streams.streamC = true;
    cfg.streams.streamD = false;

    Session s(cfg, clock, dir);
    REQUIRE(s.start(makeMeta()));
    const auto header = readAll(s.headerPath());
    REQUIRE(header.find("\"a\": true")  != std::string::npos);
    REQUIRE(header.find("\"b\": false") != std::string::npos);
    REQUIRE(header.find("\"c\": true")  != std::string::npos);
    REQUIRE(header.find("\"d\": false") != std::string::npos);

    s.stop();
    std::filesystem::remove_all(dir);
}
