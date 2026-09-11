// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.app.Dialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.res.ColorStateList
import android.os.Bundle
import android.widget.Toast
import androidx.annotation.StringRes
import androidx.core.content.ContextCompat
import androidx.fragment.app.DialogFragment
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.launch
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.databinding.DialogNextendoProfileBinding
import org.citron.citron_emu.databinding.ListItemProfileActionBinding
import org.citron.citron_emu.utils.NextendoAccountState
import org.json.JSONObject

class NextendoProfileDialogFragment : DialogFragment() {
    private var _binding: DialogNextendoProfileBinding? = null
    private val binding get() = _binding!!

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        _binding = DialogNextendoProfileBinding.inflate(layoutInflater)

        addAction(R.string.nextendo_friends, R.string.nextendo_friends_description) {
            NextendoFriendsDialogFragment().show(
                parentFragmentManager,
                NextendoFriendsDialogFragment.TAG
            )
        }
        addAction(
            R.string.nextendo_manage_cloud_saves,
            R.string.nextendo_manage_cloud_saves_description
        ) {
            NextendoCloudSavesDialogFragment().show(
                parentFragmentManager,
                NextendoCloudSavesDialogFragment.TAG
            )
        }
        addAction(R.string.nextendo_play_history, R.string.nextendo_play_history_description) {
            NextendoPlayHistoryDialogFragment().show(
                parentFragmentManager,
                NextendoPlayHistoryDialogFragment.TAG
            )
        }

        binding.textFriendCode.setOnClickListener {
            val code = binding.textFriendCode.tag as? String
            if (!code.isNullOrEmpty()) {
                copyToClipboard(code)
            }
        }
        binding.statusRow.setOnClickListener { refreshStatus() }
        binding.buttonSignOut.setOnClickListener { confirmSignOut() }

        val storedUsername = NativeLibrary.getNextendoAccountStatus()
        if (storedUsername.isNotEmpty()) {
            binding.textName.text = storedUsername
        }
        updateProfile()
        refreshStatus()
        NextendoAccountState.refresh()
        lifecycleScope.launch {
            NextendoAccountState.generation.collect { updateProfile() }
        }

        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_profile)
            .setView(binding.root)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun addAction(@StringRes title: Int, @StringRes description: Int, action: () -> Unit) {
        val row = ListItemProfileActionBinding.inflate(layoutInflater, binding.layoutActions, false)
        row.textActionTitle.setText(title)
        row.textActionDescription.setText(description)
        row.root.setOnClickListener { action() }
        binding.layoutActions.addView(row.root)
    }

    private fun updateProfile() {
        val profile = NextendoAccountState.profile ?: return
        binding.textName.text = profile.name
        binding.textConsoleNickname.visibility =
            if (profile.consoleNickname.isEmpty()) android.view.View.GONE else android.view.View.VISIBLE
        binding.textConsoleNickname.text = profile.consoleNickname
        binding.textFriendCode.text = getString(R.string.nextendo_friend_code_value, profile.friendCode)
        binding.textFriendCode.tag = profile.friendCode
        binding.imageAvatar.setImageBitmap(NextendoAccountState.avatar)
    }

    // The account gates are server-side, so the header asks the account server instead of
    // guessing from the local link state; tap retries.
    private fun refreshStatus() {
        binding.textStatus.setText(R.string.nextendo_status_checking)
        setStatusDot(R.color.nextendo_avatar_background)
        Thread {
            val json = NativeLibrary.nextendoGetOnlineStatusJson()
            post {
                val status = try {
                    JSONObject(json)
                } catch (_: Exception) {
                    null
                }
                when {
                    status == null || !status.optBoolean("queried") -> {
                        binding.textStatus.setText(R.string.nextendo_status_unknown)
                        setStatusDot(R.color.nextendo_avatar_background)
                    }

                    status.optBoolean("allow") -> {
                        binding.textStatus.setText(R.string.nextendo_online_status_ok)
                        setStatusDot(R.color.status_signed_in)
                    }

                    else -> {
                        val reason =
                            status.optString("message").ifEmpty { status.optString("reason") }
                        binding.textStatus.text = if (reason.isEmpty()) {
                            getString(R.string.nextendo_online_status_blocked_empty)
                        } else {
                            getString(R.string.nextendo_online_status_blocked, reason)
                        }
                        setStatusDot(R.color.status_not_signed_in)
                    }
                }
            }
        }.start()
    }

    private fun setStatusDot(colorRes: Int) {
        binding.statusDot.backgroundTintList =
            ColorStateList.valueOf(ContextCompat.getColor(requireContext(), colorRes))
    }

    private fun confirmSignOut() {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_sign_out)
            .setMessage(R.string.nextendo_sign_out_confirm)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                NativeLibrary.nextendoSignOut()
                NextendoAccountState.clear()
                dismiss()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun copyToClipboard(text: String) {
        val clipboard =
            requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("Nextendo", text))
        Toast.makeText(CitronApplication.appContext, R.string.nextendo_copied, Toast.LENGTH_SHORT)
            .show()
    }

    private fun post(block: () -> Unit) {
        activity?.runOnUiThread {
            if (isAdded && _binding != null) {
                block()
            }
        }
    }

    companion object {
        const val TAG = "NextendoProfileDialogFragment"
    }
}
