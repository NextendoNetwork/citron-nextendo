// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <system_error>

namespace Citron::SetupStartup {
inline bool ConfigurationExists(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size != 0;
}

// Updating the executable is not a new installation. Existing users can always
// open Tools > Setup Wizard manually, including installations without its flag.
inline bool ShouldOffer(bool configuration_existed, bool wizard_seen, bool keys,
                        bool firmware, bool game_folder) {
    return !configuration_existed && !wizard_seen && !keys && !firmware && !game_folder;
}
} // namespace Citron::SetupStartup
