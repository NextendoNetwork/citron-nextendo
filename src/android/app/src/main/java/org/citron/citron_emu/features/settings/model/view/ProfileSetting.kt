// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.features.settings.model.view

import android.graphics.Bitmap
import androidx.annotation.StringRes

// Linked-account row: avatar, username and a "signed in" line; opens the profile page.
class ProfileSetting(
    @StringRes titleId: Int = 0,
    titleString: String = "",
    @StringRes descriptionId: Int = 0,
    descriptionString: String = "",
    val avatar: Bitmap? = null,
    val runnable: () -> Unit
) : SettingsItem(emptySetting, titleId, titleString, descriptionId, descriptionString) {
    override val type = TYPE_PROFILE
}
