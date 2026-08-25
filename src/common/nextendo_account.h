// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include "common/common_types.h"

namespace Common::NextendoAccount {

// The linked Nextendo Network account. Written by the login dialog, read by the acc service so the
// NEX login presents the account's persistent principal id. Stored as key=value, not JSON, so it
// stays readable if a field is added later.

bool IsLinked();
u64 GetPid();
std::string GetUsername();
std::string GetFriendCode();
std::string GetToken();

// Bumped by every Save()/Clear(). Lets callers that cache derived data (e.g. the acc
// service's signed id_token, which embeds GetToken() in an "nnex" claim) detect a link
// state change and invalidate their cache instead of relying on a time-based expiry that
// can keep serving a token minted before the account was linked.
u64 GetGeneration();

void Save(u64 pid, std::string_view username, std::string_view friend_code,
          std::string_view token);
void Clear();

// Mirrors the link state onto the guest SD card (sdmc_root/config/nextendo/session.txt) so
// homebrew can read its signed-in identity with a plain fopen(), no BAAS id_token needed.
void WriteGuestBridge(const std::filesystem::path& sdmc_root);

} // namespace Common::NextendoAccount
