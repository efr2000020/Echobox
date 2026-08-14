// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Tests for the replay-tool session-header ingest. Only built when
/// ECHOBOX_BUILD_REPLAY=ON.
///
/// Coverage: the actual-shape header the device wrote (from
/// field_samples/collection_08-04-2026/collection/SESSION_HEADER.json)
/// round-trips into the right Config fields, upward search finds a
/// header in a parent dir, and a missing file surfaces cleanly.

#include "SessionHeader.hpp"

#include "app/Config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using echobox::app::Config;
using echobox::replay::applySessionHeader;
using echobox::replay::findSessionHeader;
using echobox::replay::readSessionHeader;

namespace {

// Byte-for-byte copy of the shape the device writes today; if the writer
// changes, this fixture is the canary that tells us the reader is stale.
constexpr const char* kFieldHeader =
    "{\n"
    "  \"firmware_sha\":    \"unknown\",\n"
    "  \"algorithm\":       \"BandEnergyDetector\",\n"
    "  \"sample_rate\":     384000,\n"
    "  \"channels\":        1,\n"
    "  \"mic_device\":      \"plughw:CARD=r0\",\n"
    "  \"site_note\":       \"\",\n"
    "  \"config\":          {\"preroll_ms\":10,\"silence_ms\":40,\"min_length_ms\":0,"
    "\"max_length_ms\":200,\"snr_threshold\":18,\"fft_size\":4096,\"hop_size\":512,"
    "\"freq_lo_hz\":16000,\"freq_hi_hz\":192000,\"cricket_filter\":true},\n"
    "  \"start_wall_iso8601\": \"2026-08-03T19:24:02.124Z\",\n"
    "  \"start_sample\":    0,\n"
    "  \"free_mb_at_start\":104324,\n"
    "  \"est_max_hours\":   39.5658,\n"
    "  \"governor\": {\n"
    "    \"max_duration_sec\":   0,\n"
    "    \"min_free_mb_floor\":  100,\n"
    "    \"poll_interval_sec\":  10\n"
    "  },\n"
    "  \"streams\": {\n"
    "    \"a\": true, \"b\": true, \"c\": true, \"d\": true\n"
    "  }\n"
    "}\n";

struct TempDir {
    fs::path path;
    TempDir()
        : path(fs::temp_directory_path()
               / ("echobox_shdr_test_" + std::to_string(std::rand()))) {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

fs::path writeHeader(const fs::path& dir, const std::string& body) {
    fs::path p = dir / "SESSION_HEADER.json";
    std::ofstream(p) << body;
    return p;
}

} // namespace

TEST_CASE("readSessionHeader: parses real device header shape", "[replay]") {
    TempDir dir;
    auto hdrPath = writeHeader(dir.path, kFieldHeader);
    auto h = readSessionHeader(hdrPath);

    REQUIRE(h.sample_rate);   CHECK(*h.sample_rate   == 384000);
    REQUIRE(h.channels);      CHECK(*h.channels      == 1);
    REQUIRE(h.algorithm);     CHECK(*h.algorithm     == "BandEnergyDetector");
    REQUIRE(h.preroll_ms);    CHECK(*h.preroll_ms    == 10u);
    REQUIRE(h.silence_ms);    CHECK(*h.silence_ms    == 40u);
    REQUIRE(h.min_length_ms); CHECK(*h.min_length_ms == 0u);
    REQUIRE(h.max_length_ms); CHECK(*h.max_length_ms == 200u);
    REQUIRE(h.snr_threshold); CHECK(*h.snr_threshold == 18.0f);
    REQUIRE(h.fft_size);      CHECK(*h.fft_size      == 4096u);
    REQUIRE(h.hop_size);      CHECK(*h.hop_size      == 512u);
    REQUIRE(h.freq_lo_hz);    CHECK(*h.freq_lo_hz    == 16000);
    REQUIRE(h.freq_hi_hz);    CHECK(*h.freq_hi_hz    == 192000);
    REQUIRE(h.cricket_filter); CHECK(*h.cricket_filter == true);
}

TEST_CASE("applySessionHeader: overrides only present fields", "[replay]") {
    TempDir dir;
    auto hdrPath = writeHeader(dir.path, kFieldHeader);
    auto h = readSessionHeader(hdrPath);

    Config cfg; // starts at shipping defaults
    // Pick a couple of shipping-vs-header divergences to sanity-check.
    REQUIRE(cfg.preRollMs == 10u);       // shipping default
    REQUIRE(cfg.freqLoHz  == 20000);     // shipping default
    REQUIRE(cfg.snrThreshold == 8.0f);   // shipping default

    const std::size_t n = applySessionHeader(h, cfg);
    CHECK(n >= 13);                       // 13 fields in the flat config
    CHECK(cfg.preRollMs    == 10u);      // header value
    CHECK(cfg.freqLoHz     == 16000);    // header value
    CHECK(cfg.snrThreshold == 18.0f);    // header value
}

TEST_CASE("findSessionHeader: walks up from a sub-dir", "[replay]") {
    TempDir dir;
    writeHeader(dir.path, kFieldHeader);
    // Simulate the collection_.../collection/reference/*.wav layout.
    auto sub = dir.path / "collection" / "reference";
    fs::create_directories(sub);
    auto found = findSessionHeader(sub);
    CHECK(found == dir.path / "SESSION_HEADER.json");
}

TEST_CASE("findSessionHeader: returns empty when absent", "[replay]") {
    TempDir dir;
    // No header written.
    CHECK(findSessionHeader(dir.path).empty());
}

TEST_CASE("readSessionHeader: tolerates unknown top-level fields", "[replay]") {
    // Newer firmware might add fields we don't know — the parser must ignore
    // them, not choke.
    TempDir dir;
    const std::string body =
        "{ \"future_field\": 42,"
        "  \"sample_rate\": 384000,"
        "  \"config\": { \"preroll_ms\": 25, \"future_knob\": 99 } }";
    auto p = writeHeader(dir.path, body);
    auto h = readSessionHeader(p);
    REQUIRE(h.sample_rate); CHECK(*h.sample_rate == 384000);
    REQUIRE(h.preroll_ms);  CHECK(*h.preroll_ms == 25u);
    CHECK(!h.silence_ms); // absent → nullopt
}
