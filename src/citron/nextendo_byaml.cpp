// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_byaml.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "citron/nextendo_save_sync.h"
#include "web_service/nextendo_api.h"

namespace Nextendo::Byaml {

namespace {

// All three regional Splatoon 2 titles share one server-side seed.
constexpr u64 kCanonicalTitleId = 0x0100F8F0000A2000ULL;

std::filesystem::path HashPath(const std::string& title_id_hex) {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir) /
           fmt::format("nextendo_byaml_hash_{}.txt", title_id_hex);
}

std::string ReadHash(const std::string& title_id_hex) {
    std::ifstream file{HashPath(title_id_hex)};
    std::string value;
    std::getline(file, value);
    return value;
}

void WriteHash(const std::string& title_id_hex, const std::string& value) {
    if (value.empty()) {
        return;
    }
    std::ofstream file{HashPath(title_id_hex)};
    if (file) {
        file << value;
    }
}

} // Anonymous namespace

bool Required(u64 title_id) {
    return title_id == 0x0100F8F0000A2000ULL || // Splatoon 2 (EU)
           title_id == 0x01003BC0000A0000ULL || // Splatoon 2 (US)
           title_id == 0x01003C700009C800ULL;   // Splatoon 2 (JP)
}

bool Installed(u64 title_id) {
    const auto path = Common::FS::GetCitronPath(Common::FS::CitronPath::NANDDir) /
                      fmt::format("system/save/bcat/{:016X}/vsdata/VSSetting_0.byaml", title_id);
    return std::filesystem::exists(path);
}

Result Ensure(u64 title_id) {
    if (!Required(title_id)) {
        return Result::Current;
    }

    const std::string title_id_hex = fmt::format("{:016X}", title_id);
    const std::string fetch_hex = fmt::format("{:016X}", kCanonicalTitleId);

    const auto zip = WebService::NextendoApi::DownloadBcatSeed(fetch_hex);
    if (zip.empty()) {
        return Installed(title_id) ? Result::Current : Result::Failed;
    }

    const std::string server_hash = WebService::NextendoApi::HashBcatSeedHex(zip);
    if (server_hash == ReadHash(title_id_hex) && Installed(title_id)) {
        return Result::Current;
    }

    const auto dest = Common::FS::GetCitronPath(Common::FS::CitronPath::NANDDir) /
                      fmt::format("system/save/bcat/{}", title_id_hex);
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);

    if (!SaveSync::ExtractZipToDirectory(zip, dest)) {
        return Result::Failed;
    }
    WriteHash(title_id_hex, server_hash);
    return Result::Installed;
}

} // namespace Nextendo::Byaml
