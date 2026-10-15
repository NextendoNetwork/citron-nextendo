// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include "common/nextendo_friends.h"
#include "common/home_menu_activity.h"

namespace Common::NextendoFriends {

inline bool IsViewerHomeMenu(const Entry& entry) {
    if (entry.app_id == Common::HomeMenuActivity::ApplicationId) return true;
    if (entry.app_id != 0) return false;
    const bool emulator = entry.platform == "citron" || entry.platform == "ryujinx" ||
                          entry.platform == "emulator";
    return entry.status > 0 && emulator && entry.app_detail == "HOME";
}

inline u64 ViewerApplicationId(const Entry& entry) {
    return IsViewerHomeMenu(entry) ? Common::HomeMenuActivity::ApplicationId : entry.app_id;
}

// MyPage reads sys_description from its viewer-only AppKeyValueStorage.
// Keep all original game keys and leave the game-facing presence untouched.
inline std::array<u8, 0xC0> MakeViewerAppField(const Entry& entry) {
    std::array<u8, 0xC0> out{};
    std::memcpy(out.data(), entry.app_field.data(), std::min(out.size(), entry.app_field.size()));
    if (entry.status == 0) return out;

    std::string platform;
    if (entry.platform == "switch") platform = "Nintendo Switch";
    else if (entry.platform == "citron") platform = "Citron";
    else if (entry.platform == "ryujinx") platform = "Ryujinx";
    else if (entry.platform == "emulator") platform = "Emulator";

    std::string description;
    if (IsViewerHomeMenu(entry)) description = Common::HomeMenuActivity::Name;
    else if (entry.app_id != 0) description = entry.app_name;
    if (!platform.empty()) {
        if (!description.empty()) description += " · ";
        description += platform;
    }
    if (description.empty()) return out;

    std::size_t used = 0;
    while (used < out.size() && out[used] != 0) {
        const auto key_start = used;
        while (used < out.size() && out[used] != 0) ++used;
        if (used == out.size()) return out; // malformed or completely full
        const std::string_view key{reinterpret_cast<const char*>(out.data() + key_start), used - key_start};
        if (key == "sys_description") return out; // retain the game's own description
        ++used;
        while (used < out.size() && out[used] != 0) ++used;
        if (used == out.size()) return out;
        ++used;
    }
    constexpr std::string_view key = "sys_description";
    if (out.size() - used <= key.size() + 2) return out;
    auto size = std::min(description.size(), out.size() - used - key.size() - 2);
    // Do not split a UTF-8 character when fitting the bounded native field.
    if (size < description.size()) {
        while (size > 0 && (static_cast<u8>(description[size]) & 0xC0) == 0x80) --size;
    }
    if (size == 0) return out;
    std::memcpy(out.data() + used, key.data(), key.size());
    used += key.size();
    out[used++] = 0;
    std::memcpy(out.data() + used, description.data(), size);
    out[used + size] = 0;
    return out;
}

} // namespace Common::NextendoFriends
