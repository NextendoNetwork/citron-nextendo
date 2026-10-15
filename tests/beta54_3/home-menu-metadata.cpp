// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cassert>
#include <cstring>
#include "core/hle/service/ns/home_menu_metadata.h"

int main() {
    FileSys::RawNACP nacp{};
    std::vector<u8> icon;
    assert(Service::NS::GetHomeMenuControl(Common::HomeMenuActivity::ApplicationId, nacp, icon));
    for (const auto& language : nacp.language_entries) {
        assert(std::string_view(language.application_name.data()) == Common::HomeMenuActivity::Name);
        assert(std::string_view(language.developer_name.data()) == "Nextendo");
        assert(language.application_name[Common::HomeMenuActivity::Name.size()] == '\0');
    }
    assert(nacp.supported_languages == 0xFFFF);
    assert(nacp.presence_group_id == Common::HomeMenuActivity::ApplicationId);
    assert(icon.size() > 100 && icon.size() < 0x20000);
    assert(icon[0] == 0xFF && icon[1] == 0xD8);
    assert(icon[icon.size() - 2] == 0xFF && icon.back() == 0xD9);
    const auto saved = nacp;
    const auto saved_icon = icon;
    assert(!Service::NS::GetHomeMenuControl(0, nacp, icon));
    assert(!Service::NS::GetHomeMenuControl(0x0100C2500FC20000ULL, nacp, icon));
    assert(std::memcmp(&saved, &nacp, sizeof(nacp)) == 0 && icon == saved_icon);
    assert(Common::HomeMenuActivity::Description[sizeof(Common::HomeMenuActivity::Description) - 1] == 0);
    assert(sizeof(Common::HomeMenuActivity::Description) <= 0xC0);
}
