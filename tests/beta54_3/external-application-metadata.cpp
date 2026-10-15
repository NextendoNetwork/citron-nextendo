// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <string_view>
#include "common/nextendo_application_metadata.h"
#include "core/hle/service/ns/external_application_metadata.h"
int main() {
    Common::NextendoFriends::PlayHistoryEntry entry;
    entry.application_id = 0x0100C2500FC20000ULL;
    std::optional<Common::NextendoFriends::PlayHistoryEntry> combined;
    Common::NextendoFriends::MergeApplicationMetadata(combined, entry);
    auto cover = entry; cover.icon = {4, 5, 6};
    Common::NextendoFriends::MergeApplicationMetadata(combined, cover);
    auto named = entry; named.name = "Resolved title";
    Common::NextendoFriends::MergeApplicationMetadata(combined, named);
    Common::NextendoFriends::MergeApplicationMetadata(combined, entry);
    assert(combined->name == "Resolved title" && combined->icon == cover.icon);
    auto other = cover; other.application_id = 1; other.name = "Wrong title";
    Common::NextendoFriends::MergeApplicationMetadata(combined, other);
    assert(combined->name == "Resolved title" && combined->application_id == entry.application_id);
    FileSys::RawNACP nacp{};
    std::vector<u8> icon;
    Service::NS::GetExternalApplicationControl(entry, nacp, icon);
    assert(std::string_view(nacp.language_entries[0].application_name.data()) ==
           "Application (0100c2500fc20000)");
    assert(icon.size() > 100 && icon[0] == 0xFF && icon[1] == 0xD8);
    assert(icon[icon.size()-2] == 0xFF && icon.back() == 0xD9);
    entry.name = "A game not installed here";
    entry.icon = {1, 2, 3}; // The decoded cache's icon must take precedence over the placeholder.
    Service::NS::GetExternalApplicationControl(entry, nacp, icon);
    assert(icon == entry.icon);
    for (const auto& lang : nacp.language_entries)
        assert(std::string_view(lang.application_name.data()) == entry.name);
    entry.name = std::string(510, 'a') + "é";
    Service::NS::GetExternalApplicationControl(entry, nacp, icon);
    assert(std::string_view(nacp.language_entries[0].application_name.data()) == std::string(510, 'a'));
    assert(nacp.supported_languages == 0xFFFF && nacp.presence_group_id == entry.application_id);
}
