// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recorder/PreRollBuffer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <array>

using namespace echobox::recorder;

TEST_CASE("PreRollBuffer: basic operations", "[recorder]") {
    PreRollBuffer buf(10);
    REQUIRE(buf.capacity() == 10);
    REQUIRE(buf.writeCount() == 0);

    SECTION("Single write and read") {
        std::array<std::int16_t, 3> data = {1, 2, 3};
        buf.write(data);
        CHECK(buf.writeCount() == 3);

        auto cursor = buf.openCursor(3);
        CHECK(cursor.pos == 0);
        CHECK(cursor.valid);

        std::array<std::int16_t, 5> dest = {0};
        std::size_t lost = 0;
        std::size_t read = buf.read(cursor, dest, lost);
        
        CHECK(read == 3);
        CHECK(lost == 0);
        CHECK(dest[0] == 1);
        CHECK(dest[1] == 2);
        CHECK(dest[2] == 3);
        CHECK(cursor.pos == 3);
    }

    SECTION("Read empty") {
        auto cursor = buf.openCursor(0);
        std::array<std::int16_t, 5> dest = {0};
        std::size_t lost = 0;
        CHECK(buf.read(cursor, dest, lost) == 0);
    }
}

TEST_CASE("PreRollBuffer: wrap around and overruns", "[recorder]") {
    PreRollBuffer buf(10);

    SECTION("Wrap around without overrun") {
        std::vector<std::int16_t> data(8, 1);
        buf.write(data);
        
        auto cursor = buf.openCursor(4);
        CHECK(cursor.pos == 4);

        std::vector<std::int16_t> moreData(4, 2);
        buf.write(moreData); // Total 12 written, capacity 10. Write head at 2 (absolute 12).
        
        // Oldest valid is 12 - 10 = 2. Cursor at 4 is still valid.
        std::array<std::int16_t, 10> dest = {0};
        std::size_t lost = 0;
        std::size_t got = buf.read(cursor, dest, lost);
        
        CHECK(got == 8); // from 4 to 12
        CHECK(lost == 0);
    }

    SECTION("Overrun detection") {
        std::vector<std::int16_t> data(5, 1);
        buf.write(data);
        
        auto cursor = buf.openCursor(5); // at 0
        
        std::vector<std::int16_t> bigData(12, 2);
        buf.write(bigData); // Total 17 written. Oldest valid is 17 - 10 = 7.
        
        // Cursor at 0 is now invalid.
        std::array<std::int16_t, 20> dest = {0};
        std::size_t lost = 0;
        std::size_t got = buf.read(cursor, dest, lost);
        
        CHECK(lost == 7); // jumped from 0 to 7
        CHECK(got == 10); // read from 7 to 17
        CHECK(cursor.pos == 17);
    }
}

TEST_CASE("PreRollBuffer: cursor logic", "[recorder]") {
    PreRollBuffer buf(100);
    
    SECTION("openCursor with more than available") {
        std::vector<std::int16_t> data(10, 1);
        buf.write(data);
        
        auto cursor = buf.openCursor(50);
        CHECK(cursor.pos == 0); // clamped to 0
    }

    SECTION("openCursor with exactly capacity") {
        std::vector<std::int16_t> data(150, 1);
        buf.write(data); // absolute write head 150. oldest valid 50.
        
        auto cursor = buf.openCursor(100);
        CHECK(cursor.pos == 50);
    }
}
