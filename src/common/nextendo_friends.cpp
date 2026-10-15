// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/nextendo_account.h"
#include "common/nextendo_friends.h"
#include "common/nextendo_application_metadata.h"

namespace Common::NextendoFriends {

namespace {
std::mutex g_mutex;
std::vector<Entry> g_entries;
s32 g_local_status = 0;
std::string g_local_app_field;
bool g_local_dirty = false;
std::chrono::steady_clock::time_point g_account_connection_time{};
u64 g_account_connection_generation{};
std::chrono::steady_clock::time_point g_local_last_push{};
// Ryujinx-Nextendo re-publishes every 45s regardless of change, to stay under the account
// server's 90s presence TTL. An edge-triggered-only push lets an unchanging presence (e.g.
// sitting in a hosted room) silently expire server-side while still active.
constexpr auto kPresenceRefreshInterval = std::chrono::seconds{45};
} // Anonymous namespace

void SetAccountConnectionAvailable(bool available) {
    const auto generation = NextendoAccount::GetGeneration();
    std::lock_guard lock{g_mutex};
    g_account_connection_generation = generation;
    g_account_connection_time = available ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
}

bool IsAccountConnectionCurrent() {
    const auto generation = NextendoAccount::GetGeneration();
    std::lock_guard lock{g_mutex};
    return g_account_connection_time != std::chrono::steady_clock::time_point{} &&
           generation == g_account_connection_generation &&
           std::chrono::steady_clock::now() - g_account_connection_time < std::chrono::seconds{90};
}

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

namespace {
struct HistorySnapshot {
    PlayHistory entries;
    std::chrono::steady_clock::time_point refreshed{};
    bool pending = false;
};
std::mutex g_history_mutex;
std::function<std::optional<PlayHistory>(u64)> g_history_resolver;
std::unordered_map<u64, HistorySnapshot> g_histories;
u64 g_history_owner = 0;
u64 g_history_generation = 0;
bool g_history_prefetch_pending = false;
} // namespace

void SetPlayHistoryResolver(std::function<std::optional<PlayHistory>(u64)> resolver) {
    std::lock_guard lock{g_history_mutex};
    g_history_resolver = std::move(resolver);
    g_histories.clear();
    ++g_history_generation;
}

PlayHistory GetPlayHistory(u64 pid) {
    const auto owner = NextendoAccount::GetPid();
    std::lock_guard lock{g_history_mutex};
    if (!NextendoAccount::IsLinked() || owner == 0 || pid == 0 || !g_history_resolver) {
        return {};
    }
    if (g_history_owner != owner) {
        g_histories.clear();
        g_history_owner = owner;
        ++g_history_generation;
    }
    auto& snapshot = g_histories[pid];
    const auto now = std::chrono::steady_clock::now();
    if (!snapshot.pending && now - snapshot.refreshed >= std::chrono::seconds{60}) {
        snapshot.pending = true;
        // Copy the resolver: no frontend object or guest memory is retained by this worker.
        std::thread{[pid, owner, generation = g_history_generation,
                     resolver = g_history_resolver] {
            auto fetched = resolver(pid);
            std::lock_guard history_lock{g_history_mutex};
            if (generation != g_history_generation || owner != NextendoAccount::GetPid()) {
                return;
            }
            auto& result = g_histories[pid];
            if (fetched) {
                result.entries = std::move(*fetched);
            }
            result.refreshed = std::chrono::steady_clock::now();
            result.pending = false;
        }}.detach();
    }
    return snapshot.entries;
}

std::optional<PlayHistoryEntry> GetApplicationMetadata(u64 application_id) {
    if (application_id == 0 || !NextendoAccount::IsLinked()) {
        return std::nullopt;
    }
    std::optional<PlayHistoryEntry> metadata;
    {
        std::lock_guard lock{g_history_mutex};
        if (g_history_owner == NextendoAccount::GetPid()) {
            for (const auto& [pid, snapshot] : g_histories) {
                for (const auto& entry : snapshot.entries) {
                    if (entry.application_id == application_id) {
                        MergeApplicationMetadata(metadata, entry);
                    }
                }
            }
        }
    }
    for (const auto& entry : Get()) {
        if (entry.app_id == application_id) {
            MergeApplicationMetadata(metadata,
                PlayHistoryEntry{application_id, 0, 0, entry.app_name, {}});
        }
    }
    return metadata;
}

void PrefetchPlayHistory(std::vector<u64> pids) {
    const auto owner = NextendoAccount::GetPid();
    {
        std::lock_guard lock{g_history_mutex};
        if (g_history_prefetch_pending || !NextendoAccount::IsLinked()) {
            return;
        }
        g_history_prefetch_pending = true;
    }
    // Warm the native profile data before MyPage caches its first response.
    // Serialize background requests rather than spawning one for every friend.
    std::thread{[owner, requested_pids = std::move(pids)] {
        for (const auto pid : requested_pids) {
            if (owner != NextendoAccount::GetPid() || !NextendoAccount::IsLinked()) {
                break;
            }
            GetPlayHistory(pid);
            for (;;) {
                {
                    std::lock_guard lock{g_history_mutex};
                    const auto it = g_histories.find(pid);
                    if (owner != NextendoAccount::GetPid() || it == g_histories.end() ||
                        !it->second.pending) {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
        }
        std::lock_guard lock{g_history_mutex};
        g_history_prefetch_pending = false;
    }}.detach();
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

namespace {
std::mutex g_name_mutex;
std::function<std::string(u64)> g_name_resolver;
std::unordered_map<u64, std::shared_future<std::string>> g_names;
std::unordered_map<u64, std::chrono::steady_clock::time_point> g_name_attempts;
} // Anonymous namespace

void SetNameResolver(std::function<std::string(u64)> resolver) {
    std::lock_guard lock{g_name_mutex};
    g_name_resolver = std::move(resolver);
}

std::string ResolveName(u64 pid, int timeout_ms) {
    std::shared_future<std::string> name;
    {
        std::lock_guard lock{g_name_mutex};
        if (!g_name_resolver || pid == 0) {
            return {};
        }
        auto it = g_names.find(pid);
        const bool failed = it != g_names.end() &&
                            it->second.wait_for(std::chrono::seconds{0}) == std::future_status::ready &&
                            it->second.get().empty();
        if (it == g_names.end() || failed) {
            const auto now = std::chrono::steady_clock::now();
            if (failed && now - g_name_attempts[pid] < std::chrono::seconds{60}) {
                return {};
            }
            // Profile batches can contain hundreds of uncached IDs. Never turn one
            // IPC into that many network waits or simultaneous background requests.
            const auto pending = std::count_if(g_names.begin(), g_names.end(), [](const auto& entry) {
                return entry.second.wait_for(std::chrono::seconds{0}) != std::future_status::ready;
            });
            if (pending >= 8) {
                return {};
            }
            g_name_attempts[pid] = now;
            it = g_names.insert_or_assign(pid, std::async(std::launch::async, g_name_resolver, pid).share())
                     .first;
        }
        name = it->second;
    }
    if (name.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        return {};
    }
    return name.get();
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
