// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Tests for the replay tool's file-backed IAudioSource. Only built when
/// ECHOBOX_BUILD_REPLAY=ON; the whole file is a no-op otherwise.

#include "WavFileAudioSource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <sndfile.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using echobox::replay::WavFileAudioSource;

namespace {

/// Write a mono int16 WAV at @p path with @p frames samples of a simple ramp.
/// Returns the path (for chaining into the params list).
fs::path writeRampWav(const fs::path& path, int sampleRate, int frames) {
    SF_INFO info{};
    info.samplerate = sampleRate;
    info.channels   = 1;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* sf = sf_open(path.c_str(), SFM_WRITE, &info);
    REQUIRE(sf != nullptr);
    std::vector<std::int16_t> samples(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        samples[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(i & 0x7FFF);
    }
    sf_count_t wrote = sf_writef_short(sf, samples.data(), frames);
    REQUIRE(wrote == frames);
    sf_close(sf);
    return path;
}

/// One-shot temp-dir RAII helper. The test writes fixture WAVs into it and
/// removes the whole tree at scope exit — keeps CI runs from accumulating
/// stray fixtures on failed asserts.
struct TempDir {
    fs::path path;
    TempDir()
        : path(fs::temp_directory_path()
               / ("echobox_replay_test_" + std::to_string(std::rand()))) {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("WavFileAudioSource: single-file playback", "[replay]") {
    TempDir dir;
    constexpr int kRate = 384000;
    constexpr int kFrames = 5000;
    writeRampWav(dir.path / "a.wav", kRate, kFrames);

    WavFileAudioSource::Params p;
    p.files              = {dir.path / "a.wav"};
    p.expectedSampleRate = kRate;
    p.expectedChannels   = 1;

    WavFileAudioSource src(std::move(p));
    src.open();

    CHECK(src.sampleRate() == kRate);
    CHECK(src.channels()   == 1);
    CHECK(src.fileCount()  == 1);
    CHECK(src.finished()   == false);

    std::array<std::int16_t, 1024> buf{};
    int totalSamples = 0;
    int reads = 0;
    while (!src.finished() && reads < 100) {
        int got = src.read(std::span<std::int16_t>(buf));
        REQUIRE(got >= 0);
        totalSamples += got;
        ++reads;
    }
    CHECK(src.finished());
    CHECK(totalSamples == kFrames);
}

TEST_CASE("WavFileAudioSource: playlist rolls across files", "[replay]") {
    TempDir dir;
    constexpr int kRate = 384000;
    writeRampWav(dir.path / "a.wav", kRate, 2000);
    writeRampWav(dir.path / "b.wav", kRate, 3000);
    writeRampWav(dir.path / "c.wav", kRate, 1000);

    auto files = echobox::replay::enumerateWavFiles(dir.path);
    REQUIRE(files.size() == 3);
    // enumerateWavFiles sorts lexicographically for reproducibility.
    CHECK(files[0].filename() == "a.wav");
    CHECK(files[1].filename() == "b.wav");
    CHECK(files[2].filename() == "c.wav");

    WavFileAudioSource::Params p;
    p.files              = files;
    p.expectedSampleRate = kRate;
    p.expectedChannels   = 1;
    WavFileAudioSource src(std::move(p));
    src.open();

    std::array<std::int16_t, 512> buf{};
    int totalSamples = 0;
    int safety = 0;
    while (!src.finished() && safety++ < 1000) {
        int got = src.read(std::span<std::int16_t>(buf));
        REQUIRE(got >= 0);
        totalSamples += got;
    }
    CHECK(src.finished());
    CHECK(totalSamples == 2000 + 3000 + 1000);
    // Post-EOF reads must return 0 (idle), not <0 (fatal). Replay driver
    // relies on this to stop cleanly without a spurious MIC_DISCONNECTED.
    CHECK(src.read(std::span<std::int16_t>(buf)) == 0);
}

TEST_CASE("WavFileAudioSource: mismatched sample rate fails open", "[replay]") {
    TempDir dir;
    writeRampWav(dir.path / "a.wav", 384000, 1000);
    writeRampWav(dir.path / "b.wav", 192000, 1000); // different rate

    WavFileAudioSource::Params p;
    p.files = {dir.path / "a.wav", dir.path / "b.wav"};
    p.expectedSampleRate = 384000;
    p.expectedChannels   = 1;
    WavFileAudioSource src(std::move(p));
    src.open(); // first file OK

    std::array<std::int16_t, 4096> buf{};
    // Drain file a; the rollover to file b should trip the sample-rate
    // check inside openCurrent(). Rollover errors surface as m_finished=true
    // (with a logged ERROR), not as an exception during read.
    int safety = 0;
    while (!src.finished() && safety++ < 100) {
        src.read(std::span<std::int16_t>(buf));
    }
    CHECK(src.finished());
}

TEST_CASE("enumerateWavFiles: missing path throws", "[replay]") {
    REQUIRE_THROWS_AS(
        echobox::replay::enumerateWavFiles("/no/such/dir/here/really"),
        std::runtime_error);
}
