// SPDX-License-Identifier: GPL-2.0-or-later
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/loader/nextendo_jd22_patch.h"

TEST_CASE("JD2022 trust patches reject conflicting images atomically", "[loader][jd22]") {
    using namespace Loader::NextendoJd22;
    SECTION("build 0") {
        std::vector<uint8_t> image(13492224);
        for (const auto& r : kBuild0)
            std::copy(r.original.begin(), r.original.end(), image.begin() + r.offset);
        auto original = image;
        const auto id = "E63D7A75EE916F07C9AAC2985A9F5CFC3114456A000000000000000000000000";
        CHECK_FALSE(Apply(0, id, "main", image));
        CHECK_FALSE(Apply(0x0100EA6014BB8000ULL, id, "sdk", image));
        CHECK_FALSE(Apply(0x0100EA6014BB8000ULL, "UNKNOWN", "main", image));
        CHECK(image == original);
        image[kBuild0[0].offset] ^= 1;
        auto conflicted = image;
        CHECK_FALSE(Apply(0x0100EA6014BB8000ULL, id, "main", image));
        CHECK(image == conflicted);
        image = original;
        REQUIRE(Apply(0x0100EA6014BB8000ULL, id, "main", image));
        for (const auto& r : kBuild0)
            CHECK(std::equal(r.data.begin(), r.data.end(), image.begin() + r.offset));
        auto patched = image;
        CHECK(Apply(0x0100EA6014BB8000ULL, id, "main", image));
        CHECK(image == patched);
        image.resize(128);
        CHECK_FALSE(Apply(0x0100EA6014BB8000ULL, id, "main", image));
    }
    SECTION("build 1") {
        std::vector<uint8_t> image(13492224);
        for (const auto& r : kBuild1)
            std::copy(r.original.begin(), r.original.end(), image.begin() + r.offset);
        auto original = image;
        const auto id = "ED8426574258F6728BE883C22010730D5D6DB531000000000000000000000000";
        CHECK_FALSE(Apply(0, id, "main", image));
        CHECK_FALSE(Apply(0x0100EA6014BB8000ULL, id, "sdk", image));
        CHECK_FALSE(Apply(0x0100EA6014BB8000ULL, "UNKNOWN", "main", image));
        CHECK(image == original);
        image[kBuild1[0].offset] ^= 1;
        auto conflicted = image;
        CHECK_FALSE(Apply(0x0100EA6014BB8000ULL, id, "main", image));
        CHECK(image == conflicted);
        image = original;
        REQUIRE(Apply(0x0100EA6014BB8000ULL, id, "main", image));
        for (const auto& r : kBuild1)
            CHECK(std::equal(r.data.begin(), r.data.end(), image.begin() + r.offset));
        auto patched = image;
        CHECK(Apply(0x0100EA6014BB8000ULL, id, "main", image));
        CHECK(image == patched);
        image.resize(128);
        CHECK_FALSE(Apply(0x0100EA6014BB8000ULL, id, "main", image));
    }
}
