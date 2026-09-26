// SPDX-FileCopyrightText: 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.features.settings.model.view

import androidx.annotation.StringRes

// Clickable sign-in row with a status pill on the right. The signed-in state is the profile
// row instead, so this row only ever renders signed out.
class SignInStatusSetting(
    @StringRes titleId: Int = 0,
    titleString: String = "",
    @StringRes descriptionId: Int = 0,
    descriptionString: String = "",
    val statusText: String = "",
    val runnable: () -> Unit
) : SettingsItem(emptySetting, titleId, titleString, descriptionId, descriptionString) {
    override val type = TYPE_SIGN_IN_STATUS
}