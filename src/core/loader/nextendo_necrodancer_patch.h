// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"

namespace Loader::NextendoNecroDancerPatch {

// Applies the built-in Crypt of the NecroDancer job-release fix to its main NSO
// ([NSOHeader][segments], the layout PatchManager::PatchNSO uses). Returns nso unchanged for any
// other title or build.
std::vector<u8> ApplyIfMatch(u64 title_id, const std::array<u8, 0x20>& build_id,
                             std::vector<u8> nso);

} // namespace Loader::NextendoNecroDancerPatch
