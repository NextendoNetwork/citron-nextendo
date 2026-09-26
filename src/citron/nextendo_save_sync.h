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

enum class Result {
    Ok,        // Pull applied the cloud save; push uploaded it.
    NoData,    // Nothing stored on the server for this title.
    LocalKept, // Pull skipped: a local save exists and force wasn't set.
    NoSaveDir, // Pull: no local save directory, and none could be created.
    Kept,      // Push: the server kept its larger save instead of this one.
    TooLarge,  // Push: over the account's cloud storage limit.
    Failed,    // Ineligible, offline, or the archive step failed.
};

// Blocking. force skips the no-overwrite check (manual "Download from Cloud").
Result Pull(Core::System& system, u64 title_id, bool force = false);

// Local I/O only -- safe to call before filesystem teardown. Empty if ineligible.
std::vector<u8> CaptureForPush(Core::System& system, u64 title_id);

// The network step for a zip from CaptureForPush -- call from a detached thread.
Result UploadCaptured(u64 title_id, std::vector<u8> zip_bytes);

// Capture + upload in one blocking call, for the manual "Upload now" action.
Result Push(Core::System& system, u64 title_id);

// Extracts a zip archive to dest. libarchive builds only (the Android target).
bool ExtractZipToDirectory(std::span<const u8> zip_data, const std::filesystem::path& dest);

} // namespace Nextendo::SaveSync
