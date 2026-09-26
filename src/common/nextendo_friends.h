// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

// Snapshot of the account's friends, refreshed by the frontend and read by the friend service.
// The service must never make a network call from an IPC handler, so it reads this instead.
namespace Common::NextendoFriends {

struct Entry {
    u64 pid = 0;
    std::string name;
    s32 status = 0;        // 0 offline, 1 online, 2 in a game
    std::string app_field; // opaque per-title presence blob, raw bytes (already base64-decoded)
    std::vector<u8> image; // profile picture, raw JPEG bytes (already base64-decoded); empty if none
    u64 account_id = 0;       // BAAS account id; 0 if the server omits it
    bool is_console = false;  // consoles advertise by account id, emulators by pid
};

void Set(std::vector<Entry> entries);
std::vector<Entry> Get();

// [Nextendo] Get() never blocks -- essential for NEX titles, which poll this in a loop from
// their own online thread; blocking there would stall PRUDP acks and the server would declare a
// communication error. But Splatoon 3 requests its friend list exactly ONCE, early in boot
// (matches Ryujinx-Nextendo's own measurement: still empty at 39s in), and never asks again --
// if the frontend's background refresh hasn't populated the cache by then, that title is stuck
// with zero friends for its whole session. GetWarm() gives that one call a short, bounded wait
// for the cache to warm up instead. Never used by NEX titles' own repeated polling.
std::vector<Entry> GetWarm(int timeout_ms);

// This player's own presence, as last set by the running game. Pushed to the account server so
// friends see them online.
// nn::friends::PresenceStatus
enum : s32 {
    PresenceOffline = 0,
    PresenceOnline = 1,
    PresenceOnlinePlay = 2,
};

void SetLocalPresence(s32 status, std::string app_field);

// Sets only the status, keeping any app_field the running game published; a title's joinable-session
// blob must survive an emulator-driven status change.
void SetLocalStatus(s32 status);
s32 GetLocalStatus();
std::string GetLocalAppField();

// Hands out the local presence when it changed, or periodically regardless of change to keep the
// account server's presence TTL from expiring while the state is unchanged (e.g. idle in a hosted
// room). False means neither condition applies yet -- don't publish.
bool TakeLocalPresenceForPublish(s32& status, std::string& app_field);

} // namespace Common::NextendoFriends
