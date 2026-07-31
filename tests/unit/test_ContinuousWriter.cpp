// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// Unit tests for the Stream-A ReferenceRing + ContinuousWriter.
///
/// Exercises push-drain-rotate against a tmp directory using synthetic
/// audio: no ALSA, no hardware. Assertions cover:
///   - drops are counted and never block the producer;
///   - chunks close at the configured cadence and open with the correct
///     sample-anchored filename;
///   - the manifest JSONL is well-formed with monotonically increasing
///     start_sample / end_sample;
///   - the recorded WAV frame counts sum to what the producer pushed
///     (minus any drops).

#include <catch2/catch_test_macros.hpp>

#include "collection/ContinuousWriter.hpp"
#include "collection/ReferenceRing.hpp"
#include "collection/SampleClock.hpp"

#include <sndfile.h>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using echobox::collection::ContinuousWriter;
using echobox::collection::ContinuousWriterConfig;
using echobox::collection::ReferenceRing;
using echobox::collection::SampleClock;

namespace {

std::filesystem::path uniqueTmpDir(const char* tag) {
    const auto base = std::filesystem::temp_directory_path();
    const auto ns   = std::chrono::steady_clock::now().time_since_epoch().count();
    auto d = base / (std::string("echobox_stream_a_test_") + tag + "_" + std::to_string(ns));
    std::filesystem::create_directories(d);
    return d;
}

std::vector<std::string> readManifestLines(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) if (!line.empty()) lines.push_back(line);
    return lines;
}

// Extract a numeric field "key": <number> from a JSON line. Good enough
// for our own small emitter; not a general parser.
std::uint64_t extractUint(const std::string& line, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    auto pos = line.find(needle);
    REQUIRE(pos != std::string::npos);
    pos += needle.size();
    std::uint64_t v = 0;
    while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos]))) {
        v = v * 10 + static_cast<std::uint64_t>(line[pos] - '0');
        ++pos;
    }
    return v;
}

std::string extractString(const std::string& line, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    auto pos = line.find(needle);
    REQUIRE(pos != std::string::npos);
    pos += needle.size();
    auto end = line.find('"', pos);
    REQUIRE(end != std::string::npos);
    return line.substr(pos, end - pos);
}

// Push a fixed pattern of samples into the ring, sleep briefly so the
// writer drains. Returns the number of samples the producer intended to
// push (drops are queryable from the ring).
std::size_t pushSquareWave(ReferenceRing& ring, SampleClock& clock,
                           std::size_t totalSamples,
                           std::size_t batchSize) {
    std::vector<std::int16_t> batch(batchSize);
    for (std::size_t i = 0; i < batch.size(); ++i) {
        batch[i] = (i & 1) ? 4000 : -4000;
    }
    std::size_t pushed = 0;
    while (pushed < totalSamples) {
        const std::size_t n = std::min(batch.size(), totalSamples - pushed);
        ring.pushBatch(std::span<const std::int16_t>(batch.data(), n));
        clock.advance(n);
        pushed += n;
        // Small yield so the writer thread can drain rather than the
        // ring going full mid-test.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pushed;
}

} // namespace


TEST_CASE("ReferenceRing: drops are counted, producer never blocks",
          "[collection][stream-a]") {
    // Tiny ring (16 samples), push 100 — 84 must be reported dropped.
    ReferenceRing ring(16);
    std::vector<std::int16_t> batch(100, 42);
    const auto dropped = ring.pushBatch(batch);
    REQUIRE(dropped == 100 - 16);
    REQUIRE(ring.dropped() == 100 - 16);
    // Drain what did fit and verify no cross-contamination.
    std::int16_t out{};
    std::size_t drained = 0;
    while (ring.popOne(out)) { REQUIRE(out == 42); ++drained; }
    REQUIRE(drained == 16);
}


TEST_CASE("ContinuousWriter: rotates chunks and writes manifest",
          "[collection][stream-a]") {
    const auto dir = uniqueTmpDir("rotate");
    SampleClock clock;

    // 8 kHz fake sample rate so we can force rotation without pushing a
    // real hour of audio. chunkDurationSec=1 → 8000 frames/chunk.
    ContinuousWriterConfig cfg;
    cfg.outputDir        = dir;
    cfg.sampleRate       = 8000;
    cfg.channels         = 1;
    cfg.chunkDurationSec = 1;

    // Ring holds 4 s of audio at 8 kHz — 32 000 samples, plenty of room.
    ReferenceRing ring(32000);

    ContinuousWriter w(cfg, ring, clock);
    w.start();

    // Push 2.5 chunks' worth of audio so we get at least 2 closed chunks.
    const std::size_t totalPush = 8000 * 25 / 10;   // 20 000 samples
    pushSquareWave(ring, clock, totalPush, 256);

    // Let the writer catch up.
    for (int i = 0; i < 100 && w.chunksClosed() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    w.stop();

    REQUIRE(w.chunksClosed() >= 2);
    REQUIRE(ring.dropped() == 0);   // ring was sized to fit

    // Manifest exists and every record is well-formed.
    const auto manifest = w.manifestPath();
    REQUIRE(std::filesystem::exists(manifest));
    const auto lines = readManifestLines(manifest);
    REQUIRE(lines.size() == w.chunksClosed());

    std::uint64_t prevEnd = 0;
    std::uint64_t sumFrames = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& L = lines[i];
        const auto startSample = extractUint(L, "start_sample");
        const auto endSample   = extractUint(L, "end_sample");
        const auto frames      = extractUint(L, "frames");
        // With zero drops the chunks must be contiguous.
        if (i > 0) REQUIRE(startSample == prevEnd);
        REQUIRE(endSample == startSample + frames);
        prevEnd = endSample;
        sumFrames += frames;

        // The referenced WAV exists and reports the same frame count.
        const auto file = extractString(L, "file");
        const auto wavPath = dir / file;
        REQUIRE(std::filesystem::exists(wavPath));
        SF_INFO info{};
        SNDFILE* sf = sf_open(wavPath.string().c_str(), SFM_READ, &info);
        REQUIRE(sf);
        CHECK(info.samplerate == 8000);
        CHECK(info.channels   == 1);
        CHECK(info.frames     == static_cast<sf_count_t>(frames));
        sf_close(sf);

        // Filename prefix is the 20-digit zero-padded start_sample so
        // `ls` sorts chronologically.
        REQUIRE(file.substr(0, 20).find_first_not_of("0123456789") == std::string::npos);
    }
    // All closed chunks together are <= what we pushed; the tail sat
    // in the in-progress chunk that stop() also closed.
    REQUIRE(sumFrames <= totalPush);
    // stop() closed the last in-progress chunk, so the sum after stop
    // exactly equals what we pushed.
    REQUIRE(sumFrames == totalPush);

    std::filesystem::remove_all(dir);
}


TEST_CASE("ContinuousWriter: stop() flushes the in-progress chunk",
          "[collection][stream-a]") {
    const auto dir = uniqueTmpDir("flush");
    SampleClock clock;

    ContinuousWriterConfig cfg;
    cfg.outputDir        = dir;
    cfg.sampleRate       = 8000;
    cfg.channels         = 1;
    cfg.chunkDurationSec = 60;   // huge — we'll stop mid-chunk

    ReferenceRing ring(32000);
    ContinuousWriter w(cfg, ring, clock);
    w.start();

    pushSquareWave(ring, clock, /* totalSamples */ 4000, /* batchSize */ 512);
    // Wait for drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    w.stop();

    const auto lines = readManifestLines(w.manifestPath());
    REQUIRE(lines.size() == 1);
    const auto endSample = extractUint(lines.front(), "end_sample");
    REQUIRE(endSample == 4000);

    std::filesystem::remove_all(dir);
}
