// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string_view>
#include "common/common_types.h"

namespace Common::HomeMenuActivity {
inline constexpr u64 ApplicationId = 0x0100000000001000ULL;
inline constexpr std::string_view Name = "qLaunch (Switch Menu)";
inline constexpr std::string_view Developer = "Nextendo";
inline constexpr char Description[] = "sys_description\0HOME Menu · Citron\0";
} // namespace Common::HomeMenuActivity
