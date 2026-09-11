// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

namespace Common::NextendoAvatar {

// The logged-in account's profile picture, as last fetched from the Nextendo account service.
// Written by the Qt profile widget's periodic refresh, read by the acc service so the emulated
// console's own profile image matches the linked Nextendo account.

void SetSelfJPEGBase64(std::string_view base64_jpeg);

// Decoded JPEG bytes, or empty if nothing has been fetched yet.
std::vector<u8> GetSelfJPEG();

// Standard base64, used for every avatar the account server hands out.
std::vector<u8> DecodeBase64(std::string_view text);

} // namespace Common::NextendoAvatar
