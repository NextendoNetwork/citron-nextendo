// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/nextendo_account.h"
#include "common/settings.h"
#include "common/string_util.h"

namespace Common::NextendoAccount {

namespace {

std::mutex g_mutex;
bool g_loaded = false;

u64 g_pid = 0;
std::string g_username;
std::string g_friend_code;
std::string g_token;
u64 g_generation = 0;

std::filesystem::path FilePath() {
    return FS::GetCitronPath(FS::CitronPath::ConfigDir) / "nextendo_account.txt";
}

// Same order acc.cpp's GetEffectivePid() uses, so a test instance presents one identity to
// both the emulated account service and the game servers.
u64 TestPid() {
    const std::string setting = Settings::values.nextendo_pid.GetValue();
    if (!setting.empty()) {
        try {
            return std::stoull(setting);
        } catch (...) {
        }
    }
    if (const char* env = std::getenv("NEXTENDO_PID"); env != nullptr && *env != '\0') {
        try {
            return std::stoull(env);
        } catch (...) {
        }
    }
    return 0;
}

std::string TestUsername(u64 pid) {
    if (const char* env = std::getenv("NEXTENDO_USERNAME"); env != nullptr && *env != '\0') {
        return env;
    }
    return fmt::format("player{}", pid); // what the servers name an unverified PID
}

// Caller holds g_mutex.
void EnsureLoaded() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;

    const std::string contents = FS::ReadStringFromFile(FilePath(), FS::FileType::TextFile);
    std::vector<std::string> lines;
    Common::SplitString(contents, '\n', lines);

    for (const auto& line : lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        const std::string key = Common::StripSpaces(line.substr(0, eq));
        const std::string value = Common::StripSpaces(line.substr(eq + 1));

        if (key == "pid") {
            try {
                g_pid = std::stoull(value);
            } catch (...) {
                g_pid = 0;
            }
        } else if (key == "username") {
            g_username = value;
        } else if (key == "friend_code") {
            g_friend_code = value;
        } else if (key == "token") {
            g_token = value;
        }
    }
}

// Caller holds g_mutex.
void WriteFile() {
    void(FS::CreateParentDirs(FilePath()));
    const std::string contents =
        fmt::format("pid={}\nusername={}\nfriend_code={}\ntoken={}\n", g_pid, g_username,
                    g_friend_code, g_token);
    void(FS::WriteStringToFile(FilePath(), FS::FileType::TextFile, contents));
}

} // Anonymous namespace

bool IsLinked() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_pid != 0;
}

u64 GetPid() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_pid;
}

std::string GetUsername() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_username;
}

std::string GetFriendCode() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_friend_code;
}

std::string GetToken() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_token;
}

u64 GetGeneration() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_generation;
}

void Save(u64 pid, std::string_view username, std::string_view friend_code,
          std::string_view token) {
    std::lock_guard lock{g_mutex};
    g_loaded = true;
    g_pid = pid;
    g_username = username;
    g_friend_code = friend_code;
    g_token = token;
    ++g_generation;
    WriteFile();
}

void Clear() {
    std::lock_guard lock{g_mutex};
    g_loaded = true;
    g_pid = 0;
    g_username.clear();
    g_friend_code.clear();
    g_token.clear();
    ++g_generation;
    void(FS::RemoveFile(FilePath()));
}

void WriteGuestBridge(const std::filesystem::path& sdmc_root) {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();

    u64 pid = g_pid;
    std::string username = g_username;
    std::string token = g_token;

    // A second local instance has no account to mirror -- there is only one browser sign-in per
    // machine -- so fall back to the same test-PID sources acc's GetEffectivePid() already
    // honours. The game servers accept a bare PID in this range while signed tokens are not yet
    // mandatory, which is what makes two-player testing on one PC possible at all.
    if (pid == 0) {
        pid = TestPid();
        if (pid != 0) {
            username = TestUsername(pid);
            token = std::to_string(pid);
        }
    }

    const auto bridge_path = sdmc_root / "config" / "nextendo" / "session.txt";
    if (pid == 0) {
        void(FS::RemoveFile(bridge_path)); // not linked -- clear any stale bridge
        return;
    }

    void(FS::CreateParentDirs(bridge_path));
    const std::string contents =
        fmt::format("pid={}\nusername={}\ntoken={}\n", pid, username, token);
    void(FS::WriteStringToFile(bridge_path, FS::FileType::TextFile, contents));
}

} // namespace Common::NextendoAccount
