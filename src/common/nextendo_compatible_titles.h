// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <unordered_map>

#include "common/common_types.h"

// Only one game version per title can reach Nextendo's servers; there's no server-side
// version-gate endpoint, so this table is the source of truth.
//
// Lives in common/, not citron/: it's read from both the Qt frontend (game-list "needs
// update" badge) and core/hle/service/acc (the actual online PID gate) -- same reason
// Common::NextendoAccount lives here instead of in either layer alone.
namespace Nextendo::CompatibleTitles {

inline const std::unordered_map<u64, std::string>& Table() {
    static const std::unordered_map<u64, std::string> table{
        {0x0100152000022000, "3.0.5"},  // Mario Kart 8 Deluxe
        {0x01006a800016e000, "13.0.4"}, // Super Smash Bros. Ultimate
        {0x0100f8f0000a2000, "5.5.2"},  // Splatoon 2 (EU)
        {0x01003bc0000a0000, "5.5.2"},  // Splatoon 2 (US)
        {0x01003c700009c800, "5.5.2"},  // Splatoon 2 (JP)
        {0x01006f8002326000, "3.0.3"},  // Animal Crossing: New Horizons
        {0x0100dca0064a6000, "1.4.0"},  // Luigi's Mansion 3
        {0x01009b500007c000, "5.5.1"},  // ARMS
        {0x0100bde00862a000, "3.1.1"},  // Mario Tennis Aces
        {0x0100c2500fc20000, "11.3.0"}, // Splatoon 3
    };
    return table;
}

inline bool IsVersionOk(u64 program_id, const std::string& installed_version) {
    const auto& table = Table();
    const auto it = table.find(program_id);
    if (it == table.end()) {
        return true;
    }
    return installed_version.empty() || installed_version == it->second;
}

} // namespace Nextendo::CompatibleTitles
