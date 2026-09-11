// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_ssbu_mods.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "citron/nextendo_save_sync.h"
#include "web_service/ssbu_mod_installer.h"

#ifdef CITRON_ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace Nextendo::SsbuMods {

namespace {

#ifdef CITRON_ENABLE_LIBARCHIVE
bool ZipContainsPathTraversal(std::span<const u8> data) {
    struct archive* a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_memory(a, data.data(), data.size()) != ARCHIVE_OK) {
        archive_read_free(a);
        return true;
    }

    bool unsafe = false;
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char* name = archive_entry_pathname(entry);
        if (name == nullptr) {
            continue;
        }
        const std::string path{name};
        if (path.starts_with('/') || path.find("..") != std::string::npos) {
            unsafe = true;
            break;
        }
    }
    archive_read_free(a);
    return unsafe;
}
#endif

std::filesystem::path FindAtmosphereDir(const std::filesystem::path& root) {
    if (root.filename() == "atmosphere") {
        return root;
    }
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory() && it->path().filename() == "atmosphere") {
            return it->path();
        }
    }
    return {};
}

} // Anonymous namespace

bool IsSsbuTitle(u64 title_id) {
    return WebService::SkylineMods::IsSsbuTitleId(title_id);
}

bool Installed(u64 title_id) {
    const auto plugins = Common::FS::GetCitronPath(Common::FS::CitronPath::SDMCDir) /
                         "atmosphere" / "contents" / fmt::format("{:016X}", title_id) /
                         "romfs" / "skyline" / "plugins";
    std::error_code ec;
    return std::filesystem::exists(plugins, ec) && !std::filesystem::is_empty(plugins, ec);
}

Outcome Install(u64 title_id) {
    Outcome outcome{};
#ifdef CITRON_ENABLE_LIBARCHIVE
    const auto sdmc_root = Common::FS::GetCitronPath(Common::FS::CitronPath::SDMCDir);
    const auto skyline_dir = sdmc_root / "atmosphere" / "contents" /
                             fmt::format("{:016X}", title_id) / "romfs" / "skyline";
    const auto plugins_dir = skyline_dir / "plugins";
    const auto cache_dir = Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir);

    std::error_code ec;
    if (std::filesystem::exists(skyline_dir, ec) &&
        !std::filesystem::is_empty(skyline_dir, ec)) {
        const auto backup_path =
            skyline_dir.parent_path() /
            fmt::format("skyline.backup_{}", static_cast<s64>(std::time(nullptr)));
        std::filesystem::rename(skyline_dir, backup_path, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy(skyline_dir, backup_path,
                                  std::filesystem::copy_options::recursive, ec);
            if (!ec) {
                std::filesystem::remove_all(skyline_dir, ec);
            }
        }
    }
    std::filesystem::create_directories(plugins_dir, ec);

    for (const auto& result : WebService::SkylineMods::FetchAllSkylineModAssets()) {
        if (!result.success || result.data.empty()) {
            outcome.failed++;
            continue;
        }

        bool ok = false;
        if (result.is_zip) {
            if (!ZipContainsPathTraversal(result.data)) {
                const auto scratch =
                    cache_dir / fmt::format("ssbu_mod_extract_{}", result.display_name);
                std::error_code rm_ec;
                std::filesystem::remove_all(scratch, rm_ec);
                if (SaveSync::ExtractZipToDirectory(result.data, scratch)) {
                    const auto atmosphere_dir = FindAtmosphereDir(scratch);
                    if (!atmosphere_dir.empty()) {
                        std::error_code copy_ec;
                        std::filesystem::copy(
                            atmosphere_dir, sdmc_root / "atmosphere",
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing,
                            copy_ec);
                        ok = !copy_ec;
                    }
                }
                std::filesystem::remove_all(scratch, rm_ec);
            }
        } else {
            std::ofstream out{plugins_dir / result.filename, std::ios::binary};
            if (out) {
                out.write(reinterpret_cast<const char*>(result.data.data()),
                          static_cast<std::streamsize>(result.data.size()));
                ok = out.good();
            }
        }

        if (ok) {
            outcome.installed++;
        } else {
            outcome.failed++;
        }
    }
#else
    (void)title_id;
#endif
    return outcome;
}

} // namespace Nextendo::SsbuMods
