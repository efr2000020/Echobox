// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/CliParser.hpp"
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>

using namespace echobox::app;

namespace {
struct Argv {
    std::vector<std::string> args;
    std::vector<char*> ptrs;

    explicit Argv(std::vector<std::string> a) : args(std::move(a)) {
        for (auto& s : args) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
    }

    int argc() const { return static_cast<int>(args.size()); }
    char** argv() { return ptrs.data(); }
};
}

TEST_CASE("CliParser: basic parsing", "[cli]") {
    Config cfg;
    Argv a({"Echobox", "--device", "hw:0,0", "--sample-rate", "192000"});
    
    auto res = parseCli(a.argc(), a.argv(), cfg);
    REQUIRE(res.kind == CliResult::Kind::Ok);
    CHECK(cfg.device == "hw:0,0");
    CHECK(cfg.sampleRate == 192000);
}

TEST_CASE("CliParser: equals-sign form", "[cli]") {
    Config cfg;
    Argv a({"Echobox", "--device=plughw:1", "--snr-threshold=15.5"});
    
    auto res = parseCli(a.argc(), a.argv(), cfg);
    REQUIRE(res.kind == CliResult::Kind::Ok);
    CHECK(cfg.device == "plughw:1");
    CHECK(cfg.snrThreshold == 15.5f);
}

TEST_CASE("CliParser: help and version", "[cli]") {
    Config cfg;
    
    SECTION("help long") {
        Argv a({"Echobox", "--help"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::HelpRequested);
    }

    SECTION("help short") {
        Argv a({"Echobox", "-h"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::HelpRequested);
    }

    SECTION("version long") {
        Argv a({"Echobox", "--version"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::VersionRequested);
    }

    SECTION("version short") {
        Argv a({"Echobox", "-V"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::VersionRequested);
    }
}

TEST_CASE("CliParser: errors", "[cli]") {
    Config cfg;

    SECTION("unknown option") {
        Argv a({"Echobox", "--no-such-flag", "value"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::Error);
        CHECK(res.errorMessage.find("unknown option") != std::string::npos);
    }

    SECTION("missing value") {
        Argv a({"Echobox", "--device"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::Error);
        CHECK(res.errorMessage.find("missing value") != std::string::npos);
    }

    SECTION("invalid integer") {
        Argv a({"Echobox", "--sample-rate", "not-a-number"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::Error);
        CHECK(res.errorMessage.find("invalid --sample-rate") != std::string::npos);
    }

    SECTION("invalid float") {
        Argv a({"Echobox", "--snr-threshold", "abc"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::Error);
        CHECK(res.errorMessage.find("invalid --snr-threshold") != std::string::npos);
    }

    SECTION("negative snr") {
        Argv a({"Echobox", "--snr-threshold", "-1.0"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::Error);
    }
}

TEST_CASE("CliParser: log level parsing", "[cli]") {
    Config cfg;
    
    SECTION("debug") {
        Argv a({"Echobox", "--log-level", "debug"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        REQUIRE(res.kind == CliResult::Kind::Ok);
        CHECK(cfg.logLevel == echobox::logging::LogLevel::Debug);
    }

    SECTION("invalid") {
        Argv a({"Echobox", "--log-level", "everything"});
        auto res = parseCli(a.argc(), a.argv(), cfg);
        CHECK(res.kind == CliResult::Kind::Error);
    }
}

TEST_CASE("CliParser: recorder options", "[cli]") {
    Config cfg;
    Argv a({"Echobox", 
            "--preroll-ms", "500", 
            "--silence-ms", "1500",
            "--min-length-ms", "100",
            "--max-length-ms", "10000"});
    
    auto res = parseCli(a.argc(), a.argv(), cfg);
    REQUIRE(res.kind == CliResult::Kind::Ok);
    CHECK(cfg.preRollMs == 500);
    CHECK(cfg.silenceMs == 1500);
    CHECK(cfg.minLengthMs == 100);
    CHECK(cfg.maxLengthMs == 10000);
}

TEST_CASE("CliParser: DSP options", "[cli]") {
    Config cfg;
    Argv a({"Echobox", 
            "--fft-size", "2048", 
            "--hop-size", "256",
            "--freq-lo-hz", "15000",
            "--freq-hi-hz", "150000"});
    
    auto res = parseCli(a.argc(), a.argv(), cfg);
    REQUIRE(res.kind == CliResult::Kind::Ok);
    CHECK(cfg.fftSize == 2048);
    CHECK(cfg.hopSize == 256);
    CHECK(cfg.freqLoHz == 15000);
    CHECK(cfg.freqHiHz == 150000);
}
