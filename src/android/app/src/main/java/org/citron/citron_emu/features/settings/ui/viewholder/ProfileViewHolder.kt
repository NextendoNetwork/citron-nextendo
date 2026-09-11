// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.features.settings.ui.viewholder

import android.view.View
import org.citron.citron_emu.databinding.ListItemProfileBinding
import org.citron.citron_emu.features.settings.model.view.ProfileSetting
import org.citron.citron_emu.features.settings.model.view.SettingsItem
import org.citron.citron_emu.features.settings.ui.SettingsAdapter
import org.citron.citron_emu.utils.ViewUtils.setVisible

class ProfileViewHolder(val binding: ListItemProfileBinding, adapter: SettingsAdapter) :
    SettingViewHolder(binding.root, adapter) {

    private lateinit var setting: ProfileSetting

    override fun bind(item: SettingsItem) {
        setting = item as ProfileSetting
        binding.textSettingName.text = item.title
        binding.textSettingDescription.setVisible(item.description.isNotEmpty())
        binding.textSettingDescription.text = item.description
        binding.imageProfile.setImageBitmap(setting.avatar)
    }

    override fun onClick(clicked: View) {
        setting.runnable.invoke()
    }

    override fun onLongClick(clicked: View): Boolean {
        return true
    }
}
