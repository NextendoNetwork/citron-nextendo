// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/nextendo_account.h"
#include "common/nextendo_friends.h"

namespace Common::NextendoFriends {

namespace {
std::mutex g_mutex;
std::vector<Entry> g_entries;
s32 g_local_status = 0;
std::string g_local_app_field;
bool g_local_dirty = false;
std::chrono::steady_clock::time_point g_local_last_push{};
// Ryujinx-Nextendo re-publishes every 45s regardless of change, to stay under the account
// server's 90s presence TTL. An edge-triggered-only push lets an unchanging presence (e.g.
// sitting in a hosted room) silently expire server-side while still active.
constexpr auto kPresenceRefreshInterval = std::chrono::seconds{45};
} // Anonymous namespace

namespace {
// [Nextendo] Mirror the friend list onto the guest's SD card, the same way
// NextendoAccount::WriteGuestBridge exposes the signed-in account. Homebrew has no route to
// the friend service's IPC, so a plain file is the only way a title like Golden Balloon can
// know who your friends are -- which is what lets it offer "race this friend's ghost"
// instead of only a global leaderboard.
//
// Written on every refresh rather than once at boot, so a friend coming online is reflected
// without restarting the game. Name goes last on each line because it is the only field that
// could contain a comma.
void WriteFriendBridgeLocked(const std::vector<Entry>& entries) {
    const auto path = FS::GetCitronPath(FS::CitronPath::SDMCDir) / "config" / "nextendo" /
                      "friends.txt";
    if (!NextendoAccount::IsLinked()) {
        void(FS::RemoveFile(path)); // signed out -- do not leave a stale list behind
        return;
    }
    void(FS::CreateParentDirs(path));

    std::string contents;
    for (const auto& entry : entries) {
        if (entry.pid == 0) {
            continue;
        }
        contents += fmt::format("{},{},{}\n", entry.pid, entry.status, entry.name);
    }
    void(FS::WriteStringToFile(path, FS::FileType::TextFile, contents));
}
} // Anonymous namespace

void Set(std::vector<Entry> entries) {
    std::lock_guard lock{g_mutex};
    g_entries = std::move(entries);
    WriteFriendBridgeLocked(g_entries);
}

std::vector<Entry> Get() {
    std::lock_guard lock{g_mutex};
    return g_entries;
}

std::vector<Entry> GetWarm(int timeout_ms) {
    auto entries = Get(); // Same background refresh Get() always relies on -- just wait for it.
    if (!entries.empty() || !Common::NextendoAccount::IsLinked()) {
        return entries;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        entries = Get();
        if (!entries.empty()) {
            break;
        }
    }
    return entries;
}

void SetLocalPresence(s32 status, std::string app_field) {
    std::lock_guard lock{g_mutex};
    if (g_local_status != status || g_local_app_field != app_field) {
        g_local_dirty = true;
    }
    g_local_status = status;
    g_local_app_field = std::move(app_field);
}

void SetLocalStatus(s32 status) {
    std::lock_guard lock{g_mutex};
    if (g_local_status != status) {
        g_local_status = status;
        g_local_dirty = true;
    }
}

s32 GetLocalStatus() {
    std::lock_guard lock{g_mutex};
    return g_local_status;
}

std::string GetLocalAppField() {
    std::lock_guard lock{g_mutex};
    return g_local_app_field;
}

bool TakeLocalPresenceForPublish(s32& status, std::string& app_field) {
    std::lock_guard lock{g_mutex};
    const auto now = std::chrono::steady_clock::now();
    if (!g_local_dirty && now - g_local_last_push < kPresenceRefreshInterval) {
        return false;
    }
    g_local_dirty = false;
    g_local_last_push = now;
    status = g_local_status;
    app_field = g_local_app_field;
    return true;
}

} // namespace Common::NextendoFriends
