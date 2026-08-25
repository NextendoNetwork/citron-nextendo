// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace WebService::NextendoApi {

struct LoginResult {
    bool ok = false;
    std::string error;       // Set when ok is false.
    u64 pid = 0;
    std::string username;
    std::string friend_code;
    std::string token;
};

// Reason a NEX login was refused, as evaluated by the account server's gates.
struct OnlineStatus {
    bool queried = false;    // False when the request itself failed (offline, no token).
    bool allow = false;
    std::string reason;      // unknown | disabled | unverified | discord_unlinked | elsewhere
    std::string message;     // Human-readable text for the reason.
};

// The API base url. NEXTENDO_API overrides it; only https or loopback is accepted, because this
// request carries the account token.
std::string BaseUrl();

// Signs in through the user's browser (OAuth loopback + PKCE), so the emulator never sees the
// e-mail or password: password login on /api/login is website-only, behind a captcha. `open_url` is
// handed the authorize URL to open. Blocks until the browser reaches the loopback callback.
LoginResult SignInWithBrowser(const std::function<void(const std::string&)>& open_url);

// Uses the stored account token. Answers only about the caller's own account.
OnlineStatus GetOnlineStatus();

struct HistoryEntry {
    std::string title_id; // 16 uppercase hex digits
    std::string name;     // may be empty; the server keeps the name it already has
    u64 seconds = 0;
    std::string last_played;  // RFC3339 UTC
    std::string icon_base64;  // may be empty
};

// Pushes play time for the account's history. The server merges, so sending only what changed is
// enough. Best-effort: a failure is logged and dropped.
void SyncHistory(const std::vector<HistoryEntry>& entries);

struct Friend {
    u64 pid = 0;
    std::string name;        // console nickname if set, else account username
    std::string friend_code;
    s32 presence_status = 0; // 0 offline, 1 online, 2 in a game
    std::string app_field;   // opaque per-title presence blob
    std::string app_id;      // running title id, 16 hex digits; empty when nothing is running
    std::string app_name;    // display name the playing client already resolved; may be empty
    std::string image_base64; // profile picture, base64 JPEG; empty if none set
};

struct FriendList {
    bool ok = false;
    std::string error;
    std::vector<Friend> friends;
    std::vector<Friend> requests; // incoming, awaiting accept/decline
};

FriendList GetFriends();

// All return an empty string on success, else a message fit to show the user.
std::string AddFriendByCode(const std::string& friend_code);
std::string AcceptFriend(u64 pid);
std::string DeclineFriend(u64 pid);
std::string RemoveFriend(u64 pid);

// Pushes the console nickname so friends and games see the account's name, not "Player".
void PushProfileName(const std::string& name);

// A player as seen in the current lobby or in the recent-encounters list. The server never
// includes an IP address in either. avatar_url identifies whether the player has a picture;
// fetch the bytes with GetAvatarByPid, which reconstructs the request path from pid rather than
// trusting this field as a request target.
struct LobbyPlayer {
    u64 pid = 0;
    std::string name;
    bool known = false; // false when pid isn't a known Nextendo account (can't add/report)
    std::string avatar_url;
    std::string friend_code;
    bool host = false;
    bool is_me = false;
    std::string title_id;
    std::string seen_at; // RFC3339 UTC; empty when not applicable (current lobby members)
};

struct Lobby {
    bool in_lobby = false;
    std::string title_id;
    std::string type;
    std::string state;      // raw label from the game server; not for display
    std::string state_code; // "searching" | "matched" | "" if the game server didn't classify
    u64 id = 0;
    int count = 0;
    int max = 0;
    std::vector<LobbyPlayer> players;
};

// The lobby the account is in right now, and who else is in it.
Lobby GetMyLobby();

// The last players encountered online, most recent first.
std::vector<LobbyPlayer> GetRecentPlayers();

// Downloads a player's avatar (base64 JPEG). Public, unauthenticated endpoint; the account token
// is never attached. Empty on failure or if the player has no avatar set.
std::string GetAvatarByPid(u64 pid);

// Reports a player. The server refuses if the account never actually shared a lobby with pid.
// Returns an empty string on success, else an error code ("not_encountered", "quota", or a
// message fit to show the user).
std::string ReportPlayer(u64 pid, const std::string& reason, const std::string& comment);

// Publishes this player's presence so friends see them online and, for titles that use it,
// joinable. app_id is the running title id; empty when nothing is running. app_name is the
// display name already resolved locally (e.g. from NACP) so a friend can show it even without
// that title in their own library; empty when nothing is running or the name couldn't be resolved.
void PushPresence(s32 status, const std::string& app_field, const std::string& app_id,
                  const std::string& app_name);

// Downloads the BCAT schedule seed (a zip of vsdata/coopdata/fesdata) for titles that need one
// locally to go online (Splatoon 2). title_id_hex is 16 uppercase hex digits. Empty on failure.
//
// Always fetches the full zip rather than relying on a conditional GET (If-Modified-Since):
// this endpoint's Last-Modified handling isn't reliable enough to trust as the sole freshness
// signal (Ryujinx-Nextendo hits the same server and has the same finding), so the caller should
// hash the returned bytes with HashBcatSeedHex and compare against what's locally installed.
std::vector<u8> DownloadBcatSeed(const std::string& title_id_hex);

// Lowercase hex SHA-256 of the given bytes, for comparing a freshly downloaded BCAT seed zip
// against the hash of what's currently installed locally.
std::string HashBcatSeedHex(const std::vector<u8>& data);

// Public, unauthenticated. Maps lowercase-hex title id -> currently connected player count.
std::map<std::string, int> GetOnlineCounts();

// Public, unauthenticated. Total players in Nazi Zombies Portable's own nextendo-nzp server.
int GetNzpOnlineCount();

// Downloads this title's cloud save. title_id_hex is 16 hex digits. nullopt when there is no
// cloud save stored yet, the account can't use cloud saves (guest), or the request failed.
std::optional<std::vector<u8>> PullSave(const std::string& title_id_hex);

// Uploads this title's save. The server rejects a drastically smaller upload against an existing
// larger save rather than clobbering it (kept=true in that case, still reported as success here).
// Returns an error message fit to show the user, or empty on success.
std::string PushSave(const std::string& title_id_hex, std::span<const u8> data);

struct Profile {
    bool ok = false;
    std::string error;
    std::string name;         // acct.Username, the authoritative account nickname
    std::string image_base64; // own profile picture, base64 JPEG; empty if none set

    // The rest of the server's Profile blob. The server replaces this blob wholesale on every
    // push (no per-field merge), so any push that changes one of these must resend the others
    // to avoid silently wiping them.
    std::string console_nickname; // profile.name, the console nickname; may be empty
    std::string mii_base64;       // profile.mii; may be empty
    std::string avatar_id;        // profile.avatar, a gallery icon id; may be empty
    std::string color_hex;        // profile.color; may be empty
};

// Uses the stored account token. Own profile only.
Profile GetProfile();

// Uploads a new profile picture (base64 JPEG), preserving the rest of the profile blob (the
// server replaces it wholesale, so this fetches the current profile first and resends it with
// just the image changed). Returns an error message, or empty on success.
std::string PushProfilePicture(const std::string& image_base64);

// Renames the account (3-16 chars: letters, digits, '_' or '-'). Returns an error message, or
// empty on success.
std::string SetUsername(const std::string& username);

struct HistoryItem {
    std::string title_id;     // 16 hex digits
    std::string name;
    std::string icon_base64;  // may be empty
    u64 seconds = 0;
    std::string last_played;  // RFC3339 UTC, may be empty if never recorded
};

struct HistoryList {
    bool ok = false;
    std::string error;
    std::vector<HistoryItem> entries;
};

HistoryList GetHistory();

// Round-trip time to the backend's /api/health in milliseconds, or nullopt on failure.
std::optional<int> PingBackend();

} // namespace WebService::NextendoApi
