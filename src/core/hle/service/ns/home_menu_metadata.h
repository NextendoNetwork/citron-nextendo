// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>
#include <vector>
#include "common/home_menu_activity.h"
#include "core/file_sys/control_metadata.h"
#include "core/hle/service/ns/home_menu_icon.h"

namespace Service::NS {
inline bool GetHomeMenuControl(u64 application_id, FileSys::RawNACP& nacp,
                               std::vector<u8>& icon) {
    if (application_id != Common::HomeMenuActivity::ApplicationId) return false;
    nacp = {};
    for (auto& language : nacp.language_entries) {
        std::memcpy(language.application_name.data(), Common::HomeMenuActivity::Name.data(),
                    Common::HomeMenuActivity::Name.size());
        std::memcpy(language.developer_name.data(), Common::HomeMenuActivity::Developer.data(),
                    Common::HomeMenuActivity::Developer.size());
    }
    nacp.supported_languages = 0xFFFF;
    nacp.presence_group_id = application_id;
    icon.assign(HomeMenuIcon.begin(), HomeMenuIcon.end());
    return true;
}
} // namespace Service::NS
