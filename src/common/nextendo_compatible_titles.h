// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <charconv>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"

// Required game version per title for Nextendo's servers. The signed game catalog
// (service-catalog/v1/game-catalog, see web_service/game_catalog) is the source of truth once a
// verified, unexpired copy is available; the built-in table below is only the fallback until then.
// The installed version must be exactly the required one, neither older nor newer: a game update
// that Nextendo hasn't prepared yet must not go online.
// The numeric comparison below only normalizes how a version is written ("1.1.1.0" == "1.1.1").
//
// Lives in common/, not citron/: it's read from both the Qt frontend (game-list "needs
// update" badge) and core/hle/service/acc (the actual online PID gate) -- same reason
// Common::NextendoAccount lives here instead of in either layer alone.
namespace Nextendo::CompatibleTitles {

inline const std::unordered_map<u64, std::string>& BuiltInTable() {
    static const std::unordered_map<u64, std::string> table{
        {0x0100152000022000, "4.0.1"},  // Mario Kart 8 Deluxe
        {0x01006a800016e000, "13.0.5"}, // Super Smash Bros. Ultimate
        {0x0100f8f0000a2000, "5.5.2"},  // Splatoon 2 (EU)
        {0x01003bc0000a0000, "5.5.2"},  // Splatoon 2 (US)
        {0x01003c700009c800, "5.5.2"},  // Splatoon 2 (JP)
        {0x01006f8002326000, "3.0.3"},  // Animal Crossing: New Horizons
        {0x0100dca0064a6000, "1.4.0"},  // Luigi's Mansion 3
        {0x01009b500007c000, "5.5.1"},  // ARMS
        {0x0100bde00862a000, "3.1.1"},  // Mario Tennis Aces
        {0x0100c9c00e25c000, "4.0.0"},  // Mario Golf: Super Rush
        {0x0100c2500fc20000, "11.3.0"}, // Splatoon 3
        {0x01009b90006dc000, "3.0.3"},  // Super Mario Maker 2
        {0x010015100b514000, "1.2.1"},  // Super Mario Bros. Wonder
        {0x0100277011f1a000, "1.0.2"},  // Super Mario Bros. 35
        {0x0100770008dd8000, "1.4.0"},  // Monster Hunter Generations Ultimate
        {0x010047700d540000, "2.0.1"},  // Clubhouse Games: 51 Worldwide Classics
        {0x0100c6f01c4f8000, "1.3.0"}, // METAL GEAR SOLID: Peace Walker - Master Collection Version (JP)
        {0x010099801c4fa000, "1.3.0"}, // METAL GEAR SOLID: Peace Walker - Master Collection Version (US)
        {0x01006fd0080b2000, "1.0.19"}, // Overcooked! 2
        {0x01006fe013472000, "1.1.1"}, // Mario Party Superstars
        {0x0100965017338000, "2.3.0"}, // Super Mario Party Jamboree
        {0x0100000000010000, "1.4.1"}, // Super Mario Odyssey
        {0x01008f6008c5e000, "4.0.0"}, // Pokémon Violet
        {0x0100a3d008c5c000, "4.0.0"}, // Pokémon Scarlet
        {0x0100f43008c44000, "2.0.2"}, // Pokémon Legends: Z-A
        {0x0100abf008968000, "1.3.2"}, // Pokémon Sword
        {0x01008db008c2c000, "1.3.2"}, // Pokémon Shield
        {0x0100c9a00ece6000, "4.2.0"}, // Nintendo 64 - Nintendo Classics
        // Nintendo Switch Online Classics: the application versions the online-classics servers are
        // written and tested for. Genesis uses the exact-build signed TLS patch;
        // its AudioOut restart fix is included in the client.
        {0x0100b3c014bda000, "3.1.1"}, // SEGA Genesis - Nintendo Switch Online
        {0x0100d870045b6000, "9.1.0"}, // Nintendo Entertainment System - Nintendo Switch Online
        {0x01008d300c50c000, "6.0.0"}, // Super Nintendo Entertainment System - Nintendo Switch Online
        {0x0100c62011050000, "4.1.0"}, // Game Boy - Nintendo Switch Online
        {0x010012f017576000, "3.4.0"}, // Game Boy Advance - Nintendo Switch Online
        {0x010019401051c000, "1.3.2"}, // Mario Strikers: Battle League
        {0x0100f9f00c696000, "1.0.15"},      // Crash Team Racing Nitro-Fueled
        {0x01001b300b9be000, "2.7.7.92380"}, // Diablo III: Eternal Collection
        {0x0100a7c01b792000, "1.1.3.0"},     // Minecraft Dungeons II (update v131072)
        {0x0100b3f000be2000, "1.3.3"},       // Pokken Tournament DX
        {0x0100de600beee000, "1.6.1"},       // Saints Row: The Third - The Full Package
        {0x0100d2f00d5c0000, "1.5.0"},       // Nintendo Switch Sports
        {0x0100a250097f0000, "01.50"},       // DRAGON BALL FighterZ
        {0x0100b04011742000, "16.0.2"},      // MONSTER HUNTER RISE
        {0x0100559011740000, "16.0.2"},      // MONSTER HUNTER RISE (Japan/Hong Kong)
        {0x01000bf0152fa000, "1.1.1"},       // Zombie Army 4: Dead War
        {0x010092a00c4b6000, "1.08.00"},     // Friday the 13th: The Game
        {0x010001701248c000, "1.0.4"},       // Super Monkey Ball Banana Mania
        {0x0100b0c013912000, "2026.19.0"},   // Among Us
        {0x0100ea6014bb8000, "327835.644926"}, // Just Dance 2022 update v131072 display version
        {0x0100b6e012ebe000, "1.1.17"},      // UNO
        {0x0100fcf002a58000, "1.13.13.765"}, // Ultimate Chicken Horse
        {0x01004cc023582000, "1.2.0"},       // SpeedRunners 2: King of Speed (update v131072)
        // The update's display version carries its build tag, so it must match as exact text.
        {0x0100cea007d08000, "4.2.1-b5668"}, // Crypt of the NecroDancer (update v720896)
        {0x01006bd001e06000, "1.0.17"},      // Minecraft: Nintendo Switch Edition
        {0x0100abd01785c000, "1.0.3"},       // Portal 2
    };
    return table;
}

// Versions accepted IN ADDITION to the one in BuiltInTable(), for the rare title whose server handles several
// game versions side by side. Deliberate exception to "one exact version per title": Among Us 18.0 and
// 19.0 are separate Impostor compatibility groups and the account service does not depend on the version.
// The UDP patch is found by code pattern, but has only been tried on 19.0, and how 18.0 writes its display
// version is not verified.
inline const std::unordered_map<u64, std::vector<std::string>>& BuiltInAlsoAccepted() {
    static const std::unordered_map<u64, std::vector<std::string>> table{
        {0x0100b0c013912000, {"2026.18.0"}}, // Among Us
        {0x0100ea6014bb8000, {"326704.600931"}}, // Just Dance 2022 base build remains supported
    };
    return table;
}

// Requirements from a verified game catalog. Published by web_service/game_catalog; read here so
// core (the acc PID gate) never waits on the network.
struct CatalogRequirements {
    u64 revision{};
    s64 expires_at{}; // Unix seconds; an expired copy no longer decides anything
    std::unordered_map<u64, std::string> table;                     // title -> required version
    std::unordered_map<u64, std::vector<std::string>> also_accepted; // title -> other accepted versions
    std::unordered_set<u64> any_version; // policy "any": every version is accepted
    std::unordered_set<u64> blocked;     // disabled, or requirements this client can't evaluate
};

namespace Detail {
inline std::mutex& CatalogMutex() {
    static std::mutex mutex;
    return mutex;
}
// Never shrunk, so references returned by Table() stay valid after a newer catalog is published.
inline std::deque<CatalogRequirements>& CatalogHistory() {
    static std::deque<CatalogRequirements> history;
    return history;
}
inline const CatalogRequirements* ActiveCatalog() {
    const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    std::scoped_lock lock{CatalogMutex()};
    auto& history = CatalogHistory();
    if (history.empty() || history.back().expires_at <= now) {
        return nullptr;
    }
    return &history.back();
}
} // namespace Detail

inline void PublishCatalog(CatalogRequirements requirements) {
    std::scoped_lock lock{Detail::CatalogMutex()};
    Detail::CatalogHistory().push_back(std::move(requirements));
}

// Revision of the catalog currently deciding, or 0 when the built-in table is in use.
inline u64 ActiveCatalogRevision() {
    const auto* catalog = Detail::ActiveCatalog();
    return catalog ? catalog->revision : 0;
}

inline const std::unordered_map<u64, std::string>& Table() {
    const auto* catalog = Detail::ActiveCatalog();
    return catalog ? catalog->table : BuiltInTable();
}

inline const std::unordered_map<u64, std::vector<std::string>>& AlsoAccepted() {
    const auto* catalog = Detail::ActiveCatalog();
    return catalog ? catalog->also_accepted : BuiltInAlsoAccepted();
}

// Splits a display version ("4.0.1", "01.50", "2.7.7.92380") into its numeric parts. Anything
// that isn't dot-separated decimal numbers gives nullopt.
inline std::optional<std::vector<u64>> ParseVersion(std::string_view version) {
    constexpr std::string_view blank = " \t";
    const auto first = version.find_first_not_of(blank);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    version = version.substr(first, version.find_last_not_of(blank) - first + 1);

    std::vector<u64> parts;
    while (true) {
        const auto dot = version.find('.');
        const std::string_view part = version.substr(0, dot);
        u64 value = 0;
        const char* const end = part.data() + part.size();
        const auto [ptr, ec] = std::from_chars(part.data(), end, value);
        if (part.empty() || ec != std::errc{} || ptr != end) {
            return std::nullopt;
        }
        parts.push_back(value);
        if (dot == std::string_view::npos) {
            return parts;
        }
        version.remove_prefix(dot + 1);
    }
}

// Numeric comparison, missing parts count as 0 ("1.1.1" == "1.1.1.0", "01.50" == "1.50").
// Returns <0, 0 or >0, or nullopt when either string isn't a plain numeric version.
inline std::optional<int> CompareVersions(std::string_view lhs, std::string_view rhs) {
    const auto a = ParseVersion(lhs);
    const auto b = ParseVersion(rhs);
    if (!a || !b) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < a->size() || i < b->size(); ++i) {
        const u64 x = i < a->size() ? (*a)[i] : 0;
        const u64 y = i < b->size() ? (*b)[i] : 0;
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

// True when the installed version is exactly the required one (numeric equality, so "1.1.1.0"
// matches "1.1.1" and "01.50" matches "1.50"). An unknown version (empty) is let through, as
// before; a version that can't be compared numerically must match the table text exactly.
inline bool IsVersionOk(u64 program_id, const std::string& installed_version) {
    const auto* catalog = Detail::ActiveCatalog();
    if (catalog && catalog->blocked.contains(program_id)) {
        return false;
    }
    if (catalog && catalog->any_version.contains(program_id)) {
        return true;
    }
    const auto& table = catalog ? catalog->table : BuiltInTable();
    const auto it = table.find(program_id);
    if (it == table.end()) {
        return true;
    }
    if (installed_version.empty() || installed_version == it->second) {
        return true;
    }
    const auto order = CompareVersions(installed_version, it->second);
    if (order.has_value() && *order == 0) {
        return true;
    }
    const auto& also_accepted = catalog ? catalog->also_accepted : BuiltInAlsoAccepted();
    const auto extra = also_accepted.find(program_id);
    if (extra != also_accepted.end()) {
        for (const std::string& accepted : extra->second) {
            const auto extra_order = CompareVersions(installed_version, accepted);
            if (installed_version == accepted || (extra_order.has_value() && *extra_order == 0)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace Nextendo::CompatibleTitles
