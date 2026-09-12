// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

// [Nextendo] Super Smash Bros. Ultimate needs a set of Skyline mods (Arcropolis, nro-hook,
// smashline, pia-interface, ...) for online play. The desktop installs them from a menu entry;
// the Android client offers them on first launch and from the game's properties.
namespace Nextendo::SsbuMods {

struct Outcome {
    int installed = 0;
    int failed = 0;
};

bool IsSsbuTitle(u64 title_id);

// True when the plugins folder already has content, so the launch hook can skip the download.
bool Installed(u64 title_id);

// Blocking. Backs up an existing Skyline folder, then downloads and merges every known mod.
Outcome Install(u64 title_id);

} // namespace Nextendo::SsbuMods
