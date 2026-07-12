// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/ConfigValidator.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace echobox::app;

TEST_CASE("ConfigValidator: valid configuration passes", "[config]") {
    Config cfg;
    // Defaults should be valid
    auto errors = validateConfig(cfg);
    CHECK(errors.empty());
}

TEST_CASE("ConfigValidator: frequency window invariants", "[config]") {
    Config cfg;
    
    SECTION("lo < hi is valid") {
        cfg.freqLoHz = 20000;
        cfg.freqHiHz = 30000;
        CHECK(validateConfig(cfg).empty());
    }

    SECTION("lo == hi is invalid") {
        cfg.freqLoHz = 30000;
        cfg.freqHiHz = 30000;
        auto errors = validateConfig(cfg);
        REQUIRE_FALSE(errors.empty());
        CHECK(errors[0].find("must be less than") != std::string::npos);
    }

    SECTION("lo > hi is invalid") {
        cfg.freqLoHz = 40000;
        cfg.freqHiHz = 30000;
        auto errors = validateConfig(cfg);
        REQUIRE_FALSE(errors.empty());
        CHECK(errors[0].find("must be less than") != std::string::npos);
    }
}

TEST_CASE("ConfigValidator: freqHiHz against Nyquist", "[config]") {
    Config cfg;

    SECTION("freqHiHz exactly at Nyquist is valid") {
        cfg.sampleRate = 768000;
        cfg.freqHiHz   = 384000;
        CHECK(validateConfig(cfg).empty());
    }

    SECTION("freqHiHz below Nyquist is valid for non-default mic") {
        cfg.sampleRate = 768000;
        cfg.freqHiHz   = 250000;
        CHECK(validateConfig(cfg).empty());
    }

    SECTION("freqHiHz above Nyquist is invalid") {
        cfg.sampleRate = 192000;       // lower-rate mic
        cfg.freqHiHz   = 192000;       // forgot to lower the default
        auto errors = validateConfig(cfg);
        REQUIRE_FALSE(errors.empty());
        CHECK(errors[0].find("exceeds Nyquist") != std::string::npos);
    }
}

TEST_CASE("ConfigValidator: recording length invariants", "[config]") {
    Config cfg;

    SECTION("min <= max is valid") {
        cfg.minLengthMs = 1000;
        cfg.maxLengthMs = 5000;
        cfg.preRollMs = 100;
        // At shipping 384 kHz / hop-512 the silence floor derives to
        // 40 ms (see checkSilenceExceedsHangover); use a comfortably
        // larger value here.
        cfg.silenceMs = 250;
        CHECK(validateConfig(cfg).empty());
    }

    SECTION("min > max is invalid") {
        cfg.minLengthMs = 6000;
        cfg.maxLengthMs = 5000;
        auto errors = validateConfig(cfg);
        REQUIRE_FALSE(errors.empty());
        CHECK(errors[0].find("cannot exceed") != std::string::npos);
    }

    SECTION("max == 0 disables cap and is valid") {
        cfg.minLengthMs = 1000;
        cfg.maxLengthMs = 0;
        CHECK(validateConfig(cfg).empty());
    }
}

TEST_CASE("ConfigValidator: recording budget invariants", "[config]") {
    Config cfg;
    
    SECTION("max >= pre-roll + silence is valid") {
        cfg.preRollMs = 1000;
        cfg.silenceMs = 2000;
        cfg.maxLengthMs = 3001;
        CHECK(validateConfig(cfg).empty());
    }

    SECTION("max exactly pre-roll + silence is valid") {
        cfg.preRollMs = 1000;
        cfg.silenceMs = 2000;
        cfg.maxLengthMs = 3000;
        CHECK(validateConfig(cfg).empty());
    }

    SECTION("max < pre-roll + silence is invalid") {
        cfg.preRollMs = 1000;
        cfg.silenceMs = 2000;
        cfg.maxLengthMs = 2999;
        auto errors = validateConfig(cfg);
        REQUIRE_FALSE(errors.empty());
        CHECK(errors[0].find("must be at least") != std::string::npos);
    }
}

TEST_CASE("ConfigValidator: silence exceeds hangover invariant "
          "(cricket-filter counter-race guard)",
          "[config]") {
    Config cfg;
    // Guarantee we're testing the silence floor in isolation, not the
    // preroll+silence budget invariant.
    cfg.preRollMs   = 50;
    cfg.maxLengthMs = 5000;

    SECTION("filter on, silence at 40 ms floor is valid") {
        cfg.cricketFilter = true;
        cfg.silenceMs     = 40;
        CHECK(validateConfig(cfg).empty());
    }

    SECTION("filter on, silence one below 40 ms floor is rejected") {
        cfg.cricketFilter = true;
        cfg.silenceMs     = 39;
        auto errors = validateConfig(cfg);
        REQUIRE_FALSE(errors.empty());
        bool foundFlag  = false;
        bool namesFloor = false;
        for (const auto& e : errors) {
            if (e.find("--silence-ms") != std::string::npos) foundFlag  = true;
            if (e.find("40ms")         != std::string::npos) namesFloor = true;
        }
        CHECK(foundFlag);
        CHECK(namesFloor);
    }

    SECTION("filter off removes the floor entirely") {
        cfg.cricketFilter = false;
        cfg.silenceMs     = 10;   // well below the on-floor
        CHECK(validateConfig(cfg).empty());
    }
}
