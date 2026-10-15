// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <charconv>
#include <iterator>
#include <cstring>
#include <string>
#include "common/nextendo_friends.h"
#include "core/file_sys/control_metadata.h"
#include "core/hle/service/ns/home_menu_icon.h"

namespace Service::NS {
// Display metadata for an actual title in the account's presence/history cache.
// The neutral Nextendo placeholder is explicitly a fallback, never a claimed game cover.
inline void GetExternalApplicationControl(const Common::NextendoFriends::PlayHistoryEntry& entry,
                                          FileSys::RawNACP& nacp, std::vector<u8>& icon) {
    std::string name = entry.name;
    if (name.empty()) {
        char id[16]{};
        const auto converted = std::to_chars(std::begin(id), std::end(id), entry.application_id, 16);
        name = "Application (" + std::string(16 - (converted.ptr - id), '0') +
               std::string(id, converted.ptr) + ")";
    }
    nacp = {};
    for (auto& language : nacp.language_entries) {
        auto size = std::min(name.size(), language.application_name.size() - 1);
        if (size < name.size()) {
            while (size > 0 && (static_cast<u8>(name[size]) & 0xC0) == 0x80) --size;
        }
        std::memcpy(language.application_name.data(), name.data(), size);
    }
    nacp.supported_languages = 0xFFFF;
    nacp.presence_group_id = entry.application_id;
    if (entry.icon.empty()) icon.assign(HomeMenuIcon.begin(), HomeMenuIcon.end());
    else icon = entry.icon;
}
} // namespace Service::NS
