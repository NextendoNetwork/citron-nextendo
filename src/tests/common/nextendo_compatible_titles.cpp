// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/nextendo_compatible_titles.h"

namespace {

constexpr u64 MarioKart8Deluxe = 0x0100152000022000;
constexpr u64 SuperSmashBrosUltimate = 0x01006a800016e000;
constexpr u64 DragonBallFighterZ = 0x0100a250097f0000;
constexpr u64 Diablo3 = 0x01001b300b9be000;
constexpr u64 AmongUs = 0x0100b0c013912000;
constexpr u64 Uno = 0x0100b6e012ebe000;
constexpr u64 Minecraft = 0x01006bd001e06000;
constexpr u64 Portal2 = 0x0100abd01785c000;
constexpr u64 UltimateChickenHorse = 0x0100fcf002a58000;
constexpr u64 SpeedRunners2 = 0x01004cc023582000;
constexpr u64 Nes = 0x0100d870045b6000;
constexpr u64 Snes = 0x01008d300c50c000;
constexpr u64 GameBoy = 0x0100c62011050000;
constexpr u64 GameBoyAdvance = 0x010012f017576000;
constexpr u64 Genesis = 0x0100b3c014bda000;
constexpr u64 Nintendo64 = 0x0100c9a00ece6000;
constexpr u64 NotTracked = 0x0100000000ffffff;

} // Anonymous namespace

TEST_CASE("Nextendo::CompatibleTitles::CompareVersions", "[common][nextendo]") {
    using Nextendo::CompatibleTitles::CompareVersions;

    REQUIRE(CompareVersions("4.0.1", "4.0.1") == 0);
    REQUIRE(CompareVersions("4.0.0", "4.0.1") < 0);
    REQUIRE(CompareVersions("4.0.2", "4.0.1") > 0);
    REQUIRE(CompareVersions("4.1.0", "4.0.9") > 0);
    REQUIRE(CompareVersions("10.0.0", "9.9.9") > 0); // Numeric, not lexicographic.
    REQUIRE(CompareVersions("3.10.0", "3.9.0") > 0);
    REQUIRE(CompareVersions("1.1.1.0", "1.1.1") == 0); // Missing parts count as 0.
    REQUIRE(CompareVersions("1.1.1", "1.1.1.1") < 0);
    REQUIRE(CompareVersions("01.50", "1.50") == 0); // Leading zeros.
    REQUIRE(CompareVersions("1.08.00", "1.8") == 0);
    REQUIRE(CompareVersions(" 4.0.1 ", "4.0.1") == 0);
    REQUIRE(CompareVersions("2.7.7.92381", "2.7.7.92380") > 0);

    REQUIRE_FALSE(CompareVersions("", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("v4.0.1", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("4.0.1b", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("4..1", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("4.0.", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("-4.0.1", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("99999999999999999999.0", "4.0.1").has_value());
}

TEST_CASE("Nextendo::CompatibleTitles::IsVersionOk accepts only the exact required version",
          "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;
    using Nextendo::CompatibleTitles::Table;

    REQUIRE(Table().at(MarioKart8Deluxe) == "4.0.1");
    REQUIRE(IsVersionOk(MarioKart8Deluxe, "4.0.1"));
    REQUIRE(IsVersionOk(MarioKart8Deluxe, "4.0.1.0"));
    REQUIRE(IsVersionOk(MarioKart8Deluxe, " 4.0.1 "));
    // A newer update that Nextendo hasn't prepared yet is refused, like an older one.
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "4.0.2"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "4.1.0"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "5.0.0"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "4.0.0"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "3.0.5"));

    REQUIRE(IsVersionOk(SuperSmashBrosUltimate, "13.0.5"));
    REQUIRE_FALSE(IsVersionOk(SuperSmashBrosUltimate, "13.0.6"));
    REQUIRE_FALSE(IsVersionOk(SuperSmashBrosUltimate, "13.0.4"));
    REQUIRE_FALSE(IsVersionOk(SuperSmashBrosUltimate, "9.0.0"));

    REQUIRE(IsVersionOk(DragonBallFighterZ, "01.50"));
    REQUIRE(IsVersionOk(DragonBallFighterZ, "1.50"));
    REQUIRE_FALSE(IsVersionOk(DragonBallFighterZ, "01.51"));
    REQUIRE_FALSE(IsVersionOk(DragonBallFighterZ, "01.49"));

    REQUIRE(IsVersionOk(Diablo3, "2.7.7.92380"));
    REQUIRE_FALSE(IsVersionOk(Diablo3, "2.7.8"));
    REQUIRE_FALSE(IsVersionOk(Diablo3, "2.7.7"));
}

TEST_CASE("Nextendo::CompatibleTitles::IsVersionOk edge cases", "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;

    // Unknown installed version and untracked titles are let through, as before.
    REQUIRE(IsVersionOk(MarioKart8Deluxe, ""));
    REQUIRE(IsVersionOk(NotTracked, "0.0.1"));
    REQUIRE(IsVersionOk(NotTracked, "anything"));

    // A version that can't be compared numerically never passes unless it matches exactly.
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "v4.0.2"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "4.0.1-beta"));
}

TEST_CASE("Nextendo::CompatibleTitles table versions are all numeric", "[common][nextendo]") {
    for (const auto& [program_id, version] : Nextendo::CompatibleTitles::Table()) {
        INFO("program_id=" << program_id << " version=" << version);
        REQUIRE(Nextendo::CompatibleTitles::ParseVersion(version).has_value());
        REQUIRE(Nextendo::CompatibleTitles::IsVersionOk(program_id, version));
    }
}

TEST_CASE("Nextendo::CompatibleTitles Among Us accepts 2026.19.0 and 2026.18.0 only",
          "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;

    REQUIRE(Nextendo::CompatibleTitles::Table().at(AmongUs) == "2026.19.0");
    REQUIRE(IsVersionOk(AmongUs, "2026.19.0"));
    // 18.0 is accepted too: the server handles both side by side (separate Impostor groups).
    REQUIRE(IsVersionOk(AmongUs, "2026.18.0"));
    REQUIRE(IsVersionOk(AmongUs, "2026.18.0.0"));
    REQUIRE(IsVersionOk(AmongUs, " 2026.18.0 "));
    REQUIRE_FALSE(IsVersionOk(AmongUs, "2026.17.4"));
    REQUIRE_FALSE(IsVersionOk(AmongUs, "2026.18.1"));
    REQUIRE_FALSE(IsVersionOk(AmongUs, "2026.19.1"));
    REQUIRE_FALSE(IsVersionOk(AmongUs, "2026.20.0"));
    REQUIRE_FALSE(IsVersionOk(AmongUs, "19.0.0"));

    // The exception is not contagious: other titles keep their single exact version.
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "2026.18.0"));
    REQUIRE(Nextendo::CompatibleTitles::AlsoAccepted().size() == 1);
}

TEST_CASE("Nextendo::CompatibleTitles extra accepted versions are numeric and tracked titles",
          "[common][nextendo]") {
    for (const auto& [program_id, versions] : Nextendo::CompatibleTitles::AlsoAccepted()) {
        REQUIRE(Nextendo::CompatibleTitles::Table().contains(program_id));
        for (const auto& version : versions) {
            REQUIRE(Nextendo::CompatibleTitles::ParseVersion(version).has_value());
            REQUIRE(Nextendo::CompatibleTitles::IsVersionOk(program_id, version));
        }
    }
}

TEST_CASE("Nextendo::CompatibleTitles UNO needs exactly 1.1.17", "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;

    REQUIRE(Nextendo::CompatibleTitles::Table().at(Uno) == "1.1.17");
    REQUIRE(IsVersionOk(Uno, "1.1.17"));
    REQUIRE(IsVersionOk(Uno, "1.1.17.0"));
    REQUIRE_FALSE(IsVersionOk(Uno, "1.1.16"));
    REQUIRE_FALSE(IsVersionOk(Uno, "1.1.18"));
    REQUIRE_FALSE(IsVersionOk(Uno, "1.2.0"));
}

TEST_CASE("Nextendo::CompatibleTitles Minecraft needs exactly 1.0.17", "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;
    REQUIRE(Nextendo::CompatibleTitles::Table().at(Minecraft) == "1.0.17");
    REQUIRE(IsVersionOk(Minecraft, "1.0.17"));
    REQUIRE_FALSE(IsVersionOk(Minecraft, "1.0.16"));
    REQUIRE_FALSE(IsVersionOk(Minecraft, "1.0.0"));
}

TEST_CASE("Nextendo::CompatibleTitles Portal 2 needs exactly 1.0.3", "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;
    REQUIRE(Nextendo::CompatibleTitles::Table().at(Portal2) == "1.0.3");
    REQUIRE(IsVersionOk(Portal2, "1.0.3"));
    REQUIRE_FALSE(IsVersionOk(Portal2, "1.0.0"));
    REQUIRE_FALSE(IsVersionOk(Portal2, "1.0.2"));
}

TEST_CASE("Nextendo::CompatibleTitles Ultimate Chicken Horse needs exactly 1.13.13.765",
          "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;

    REQUIRE(Nextendo::CompatibleTitles::Table().at(UltimateChickenHorse) == "1.13.13.765");
    REQUIRE(IsVersionOk(UltimateChickenHorse, "1.13.13.765"));
    REQUIRE_FALSE(IsVersionOk(UltimateChickenHorse, "1.13.13.764"));
    REQUIRE_FALSE(IsVersionOk(UltimateChickenHorse, "1.13.14.0"));
    REQUIRE_FALSE(IsVersionOk(UltimateChickenHorse, "1.13.13"));
}

TEST_CASE("Nextendo::CompatibleTitles SpeedRunners 2 needs exactly 1.2.0", "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;

    REQUIRE(Nextendo::CompatibleTitles::Table().at(SpeedRunners2) == "1.2.0");
    REQUIRE(IsVersionOk(SpeedRunners2, "1.2.0"));
    REQUIRE(IsVersionOk(SpeedRunners2, "1.2.0.0"));
    // The base game is 1.0.0 and the in-game label of the update (v1.3.9) is not its display version.
    REQUIRE_FALSE(IsVersionOk(SpeedRunners2, "1.0.0"));
    REQUIRE_FALSE(IsVersionOk(SpeedRunners2, "1.1.0"));
    REQUIRE_FALSE(IsVersionOk(SpeedRunners2, "1.2.1"));
    REQUIRE_FALSE(IsVersionOk(SpeedRunners2, "1.3.9"));
}

TEST_CASE("Nextendo::CompatibleTitles NSO Classics need their exact application version",
          "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;
    using Nextendo::CompatibleTitles::Table;

    REQUIRE(Table().at(Nes) == "9.1.0");
    REQUIRE(Table().at(Snes) == "6.0.0");
    REQUIRE(Table().at(GameBoy) == "4.1.0");
    REQUIRE(Table().at(GameBoyAdvance) == "3.4.0");
    REQUIRE(Table().at(Nintendo64) == "4.2.0");

    REQUIRE(IsVersionOk(Nes, "9.1.0"));
    REQUIRE_FALSE(IsVersionOk(Nes, "9.1.1"));
    REQUIRE_FALSE(IsVersionOk(Snes, "5.0.0"));
    REQUIRE_FALSE(IsVersionOk(GameBoy, "1.0.0")); // the base installation is not the tested client
    REQUIRE_FALSE(IsVersionOk(GameBoyAdvance, "3.4.1"));

    REQUIRE(Table().at(Genesis) == "3.1.1");
    REQUIRE(IsVersionOk(Genesis, "3.1.1"));
    REQUIRE_FALSE(IsVersionOk(Genesis, "3.1.2"));
}

TEST_CASE("Nextendo::CompatibleTitles Just Dance accepts both reviewed builds",
          "[common][nextendo]") {
    constexpr u64 JustDance2022 = 0x0100ea6014bb8000;
    REQUIRE(Nextendo::CompatibleTitles::Table().at(JustDance2022) == "327835.644926");
    REQUIRE(Nextendo::CompatibleTitles::IsVersionOk(JustDance2022, "327835.644926"));
    REQUIRE(Nextendo::CompatibleTitles::IsVersionOk(JustDance2022, "326704.600931"));
    REQUIRE_FALSE(Nextendo::CompatibleTitles::IsVersionOk(JustDance2022, "327835.644927"));
}
