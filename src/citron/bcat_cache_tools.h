// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <system_error>
#include "common/common_types.h"

namespace Citron::BcatCacheTools {
constexpr u64 DownloadTitle(u64 title) {
    // Only Splatoon 2's three regional variants share the canonical schedule.
    return title == 0x01003bc0000a0000ULL || title == 0x01003c700009c800ULL
               ? 0x0100f8f0000a2000ULL : title;
}

// All paths are sibling directories selected by the caller. The old directory is
// retained as a backup; an incomplete stage cannot replace it.
inline bool CommitStage(const std::filesystem::path& stage, const std::filesystem::path& target,
                        const std::filesystem::path& backup) {
    std::error_code ec;
    if (!std::filesystem::is_directory(stage, ec) || ec || std::filesystem::is_empty(stage, ec) || ec)
        return false;
    if (std::filesystem::exists(backup, ec) || ec) return false;
    const bool had_cache = std::filesystem::exists(target, ec);
    if (ec) return false;
    if (had_cache) {
        std::filesystem::rename(target, backup, ec);
        if (ec) return false;
    }
    std::filesystem::rename(stage, target, ec);
    if (!ec) return true;
    if (had_cache) {
        std::error_code restore_error;
        std::filesystem::rename(backup, target, restore_error);
    }
    return false;
}
} // namespace Citron::BcatCacheTools
