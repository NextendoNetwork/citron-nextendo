// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"

// [Nextendo] Applies the account's Mii to the console's Mii database without a running game.
// A Mii travels as the raw 0x44-byte StoreData the server keeps in profile.mii -- the same
// format Ryujinx uses -- so an account Mii works on every Nextendo install.
namespace Nextendo::Mii {

enum class Result {
    Applied,  // Added to (or replaced in) the console's Mii database.
    Removed,  // Deleted from the console's Mii database.
    NotFound, // Remove: this create id isn't in the database.
    Invalid,  // Not a valid StoreData payload.
    Failed,   // Database error.
};

Result Apply(std::span<const u8> store_data);
Result Remove(std::span<const u8> store_data);

// A fresh randomized Mii as StoreData bytes; empty on failure.
std::vector<u8> Create();

} // namespace Nextendo::Mii
