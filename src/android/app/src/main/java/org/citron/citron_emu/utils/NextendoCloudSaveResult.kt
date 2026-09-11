// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.utils

import androidx.annotation.StringRes
import org.citron.citron_emu.R

// Maps the pull/push result codes from nextendo_jni.cpp to user-facing strings.
object NextendoCloudSaveResult {

    @StringRes
    fun pull(code: String): Int = when (code) {
        "applied" -> R.string.nextendo_cloud_save_applied
        "kept" -> R.string.nextendo_cloud_save_kept_local
        "none" -> R.string.nextendo_cloud_save_none
        "disabled" -> R.string.nextendo_cloud_save_disabled
        else -> R.string.nextendo_cloud_save_failed
    }

    @StringRes
    fun push(code: String): Int = when (code) {
        "uploaded" -> R.string.nextendo_cloud_save_uploaded
        "none" -> R.string.nextendo_cloud_save_none
        "disabled" -> R.string.nextendo_cloud_save_disabled
        else -> R.string.nextendo_cloud_save_failed
    }
}
