// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"

namespace Loader::NextendoDungeons2Patch {

// Applies the built-in Minecraft Dungeons II listen-host patch to its main NSO
// ([NSOHeader][segments], the layout PatchManager::PatchNSO uses). It runs before mod patches so
// a test mod can still be layered on top. Returns nso unchanged for any other title or build.
std::vector<u8> ApplyIfMatch(u64 title_id, const std::array<u8, 0x20>& build_id,
                             std::vector<u8> nso);

} // namespace Loader::NextendoDungeons2Patch
