// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

// Cloud save sync against the Nextendo account server (mirrors NextendoSaveSync.cs). Requires a
// linked account and a title in Nextendo::CompatibleTitles. Best-effort: failures are logged.
namespace Nextendo::SaveSync {

// Blocking. force skips the no-overwrite check (manual "Download from Cloud").
void Pull(Core::System& system, u64 title_id, bool force = false);

// Local I/O only -- safe to call before filesystem teardown. Empty if ineligible.
std::vector<u8> CaptureForPush(Core::System& system, u64 title_id);

// The network step for a zip from CaptureForPush -- call from a detached thread.
void UploadCaptured(u64 title_id, std::vector<u8> zip_bytes);

// Extracts a zip archive to dest. libarchive builds only (the Android target).
bool ExtractZipToDirectory(std::span<const u8> zip_data, const std::filesystem::path& dest);

} // namespace Nextendo::SaveSync
