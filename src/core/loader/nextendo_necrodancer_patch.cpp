// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <string_view>

#include "common/hex_util.h"
#include "common/logging.h"
#include "core/loader/nextendo_necrodancer_patch.h"

namespace Loader::NextendoNecroDancerPatch {

namespace {

constexpr u64 kTitleId = 0x0100CEA007D08000ULL;
constexpr std::string_view kBuildId = "92C31D164DA408747CF225F94CAEE42EDC880CBE"; // v1.4.2.1
constexpr std::size_t kNsoHeaderSize = 0x100;

// A worker hands main a job (main+0x297cd0), locks the job's mutex and waits for it. Whichever of
// the two drops the job last releases that lock in the job's on_zero_shared (main+0x29f650). When
// that is main, it unlocks a mutex the worker owns and nn::os aborts (2162-0001). Hardware timing
// keeps main ahead; the JIT's first pass through this code does not. The mutex is private to the
// job and is freed right after, so the unlock is dropped: BL std::mutex::unlock -> NOP.
constexpr std::size_t kUnlockOffset = 0x29f674;
constexpr std::array<u8, 4> kUnlockCall{{0x6F, 0xA0, 0x1C, 0x94}};
constexpr std::array<u8, 4> kNop{{0x1F, 0x20, 0x03, 0xD5}};

} // namespace

std::vector<u8> ApplyIfMatch(u64 title_id, const std::array<u8, 0x20>& build_id,
                             std::vector<u8> nso) {
    if (title_id != kTitleId) {
        return nso;
    }
    const auto build_id_raw = Common::HexToString(build_id);
    if (build_id_raw.substr(0, build_id_raw.find_last_not_of('0') + 1) != kBuildId) {
        return nso;
    }
    const std::size_t offset = kNsoHeaderSize + kUnlockOffset;
    if (nso.size() < offset + kNop.size() ||
        !std::equal(kUnlockCall.begin(), kUnlockCall.end(), nso.begin() + offset)) {
        LOG_ERROR(Loader, "[Nextendo] NecroDancer: job release not found, fix skipped");
        return nso;
    }
    std::memcpy(nso.data() + offset, kNop.data(), kNop.size());
    LOG_INFO(Loader, "[Nextendo] NecroDancer: built-in job release fix applied");
    return nso;
}

} // namespace Loader::NextendoNecroDancerPatch
