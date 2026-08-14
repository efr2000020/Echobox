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

    // Floor is ceil(8 * frame_ms) + poll + safety = ceil(8 * 1.333) + 1 + 4
    // = 16 ms at the shipped 384 kHz / hop-512. Was 40 ms before A6 cut the
    // arbitrary 24 ms padding in the 0.4.0 short-clip round.
    SECTION("filter on, silence at 16 ms floor is valid") {
        cfg.cricketFilter = true;
        cfg.silenceMs     = 16;
        CHECK(validateConfig(cfg).empty());
    }

    SECTION("filter on, silence one below 16 ms floor is rejected") {
        cfg.cricketFilter = true;
        cfg.silenceMs     = 15;
        auto errors = validateConfig(cfg);
        REQUIRE_FALSE(errors.empty());
        bool foundFlag  = false;
        bool namesFloor = false;
        for (const auto& e : errors) {
            if (e.find("--silence-ms") != std::string::npos) foundFlag  = true;
            if (e.find("16ms")         != std::string::npos) namesFloor = true;
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

TEST_CASE("ConfigValidator: shipped short-clip defaults validate cleanly",
          "[config]") {
    // Regression guard: the defaults in Config.hpp must satisfy every
    // cross-flag invariant, including the derived 16 ms silence floor and
    // the preroll+silence recording budget under the 40 ms cap.
    Config cfg;
    CHECK(cfg.preRollMs   == 10u);
    CHECK(cfg.silenceMs   == 20u);
    CHECK(cfg.maxLengthMs == 40u);
    CHECK(validateConfig(cfg).empty());

    // The cricket filter ships OFF: the sweep-shape gate was measured
    // rejecting most real bats (26 % vs 99 % per-call recall over two
    // field nights), which capped end-to-end recall far below the 90 %
    // product target. Interim, pending the gate redesign — if a future
    // round flips this back, it should be a deliberate edit here too.
    CHECK(cfg.cricketFilter == false);

    // The default geometry must stay legal for an operator who turns the
    // filter back on with --cricket-filter on. Without this the silence
    // floor above goes untested at the shipped defaults, because the
    // check short-circuits whenever the filter is off.
    Config withFilter = cfg;
    withFilter.cricketFilter = true;
    CHECK(validateConfig(withFilter).empty());

    // Product constraint: produced clips stay as short as possible and
    // never exceed 50 ms end to end. maxLengthMs is the end-to-end cap
    // (pre-roll + active + trailing silence), so this single bound is
    // what keeps the shipped geometry inside the ceiling. Detector
    // sensitivity retunes (band_snr_threshold, max_flatness) change how
    // OFTEN clips open, never how LONG they are — this guard fails if a
    // future round tries to buy recall by lengthening clips instead.
    CHECK(cfg.maxLengthMs <= 50u);
    CHECK(cfg.maxLengthMs >= cfg.preRollMs + cfg.silenceMs);
}

TEST_CASE("ConfigValidator: preroll+silence budget under short-clip defaults",
          "[config]") {
    Config cfg;
    cfg.preRollMs = 50;
    cfg.silenceMs = 50;

    SECTION("max=200 leaves exactly preroll+silence room") {
        cfg.maxLengthMs = 200;
        CHECK(validateConfig(cfg).empty());
    }

    SECTION("max=80 undershoots preroll+silence and is rejected") {
        cfg.maxLengthMs = 80;
        auto errors = validateConfig(cfg);
        REQUIRE_FALSE(errors.empty());
        bool found = false;
        for (const auto& e : errors) {
            if (e.find("--max-length-ms") != std::string::npos
                && e.find("--preroll-ms") != std::string::npos) found = true;
        }
        CHECK(found);
    }
}
