// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"

namespace Loader::NextendoPvZBfNPatch {

// Replaces PvZ BfN's built-in GOS 2013 CA key (base and 1.0.4); takes [NSOHeader][segments].
std::vector<u8> ApplyIfMatch(u64 title_id, const std::array<u8, 0x20>& build_id,
                             std::vector<u8> nso);

} // namespace Loader::NextendoPvZBfNPatch
