// SPDX-FileCopyrightText: 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.features.settings.ui.viewholder

import android.content.res.ColorStateList
import android.graphics.Color
import android.view.View
import org.citron.citron_emu.R
import org.citron.citron_emu.databinding.ListItemSignInBinding
import org.citron.citron_emu.features.settings.model.view.SettingsItem
import org.citron.citron_emu.features.settings.model.view.SignInStatusSetting
import org.citron.citron_emu.features.settings.ui.SettingsAdapter
import org.citron.citron_emu.utils.ViewUtils.setVisible

class SignInViewHolder(val binding: ListItemSignInBinding, adapter: SettingsAdapter) :
    SettingViewHolder(binding.root, adapter) {

    private lateinit var setting: SignInStatusSetting

    override fun bind(item: SettingsItem) {
        setting = item as SignInStatusSetting
        binding.textSettingName.text = item.title
        binding.textSettingDescription.setVisible(item.description.isNotEmpty())
        binding.textSettingDescription.text = item.description

        binding.chipStatus.text = setting.statusText
        val color = if (setting.signedIn) {
            R.color.status_signed_in
        } else {
            R.color.status_not_signed_in
        }
        binding.chipStatus.setChipBackgroundColor(
            ColorStateList.valueOf(binding.root.context.getColor(color))
        )
        binding.chipStatus.setTextColor(Color.WHITE)
    }

    override fun onClick(clicked: View) {
        setting.runnable.invoke()
    }

    override fun onLongClick(clicked: View): Boolean {
        // no-op
        return true
    }
}