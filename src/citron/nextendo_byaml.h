// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

// [Nextendo] Splatoon 2 refuses online play without its BCAT rotation schedule (vsdata), which
// the account server seeds. The desktop installs it before boot; this is the same download for
// the Android client.
namespace Nextendo::Byaml {

enum class Result {
    Current,   // Nothing to do: the installed data already matches the server's.
    Installed, // Fresh data was downloaded and extracted.
    Failed,    // Download or extraction failed; whatever was installed stays as-is.
};

bool Required(u64 title_id);
bool Installed(u64 title_id);

// Blocking, one small zip. Fetches and hash-compares every call, like the desktop does.
Result Ensure(u64 title_id);

} // namespace Nextendo::Byaml
