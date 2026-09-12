// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.app.Dialog
import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.net.Uri
import android.os.Bundle
import android.util.Base64
import android.widget.Toast
import androidx.annotation.StringRes
import androidx.appcompat.app.AlertDialog
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.launch
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.databinding.DialogNextendoProfileBinding
import org.citron.citron_emu.databinding.DialogNextendoUsernameBinding
import org.citron.citron_emu.databinding.ListItemProfileActionBinding
import org.citron.citron_emu.utils.NextendoAccountState
import org.json.JSONObject

class NextendoProfileDialogFragment : NextendoDialogFragment<DialogNextendoProfileBinding>() {
    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        inflateBinding { DialogNextendoProfileBinding.inflate(it) }

        addAction(
            R.string.nextendo_change_username,
            R.string.nextendo_change_username_description
        ) { showChangeUsername() }
        addAction(R.string.nextendo_mii, R.string.nextendo_mii_description) { showMiiDialog() }
        addAction(R.string.nextendo_friends, R.string.nextendo_friends_description) {
            NextendoFriendsDialogFragment().show(
                parentFragmentManager,
                NextendoFriendsDialogFragment.TAG
            )
        }
        addAction(R.string.nextendo_play_history, R.string.nextendo_play_history_description) {
            NextendoPlayHistoryDialogFragment().show(
                parentFragmentManager,
                NextendoPlayHistoryDialogFragment.TAG
            )
        }
        addAction(
            R.string.nextendo_website_profile,
            R.string.nextendo_website_profile_description
        ) { openWebsiteProfile() }

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

    // The server enforces the same rule; checking here avoids a round trip for an obvious typo.
    private fun showChangeUsername() {
        val dialogBinding = DialogNextendoUsernameBinding.inflate(layoutInflater)
        val dialog = MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_change_username)
            .setView(dialogBinding.root)
            .setPositiveButton(android.R.string.ok, null)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val username = dialogBinding.editUsername.text?.toString()?.trim().orEmpty()
                if (!username.matches(USERNAME_PATTERN)) {
                    dialogBinding.layoutUsername.error = getString(R.string.nextendo_username_invalid)
                    return@setOnClickListener
                }
                dialogBinding.layoutUsername.error = null
                dialog.dismiss()
                changeUsername(username)
            }
        }
        dialog.show()
    }

    private fun changeUsername(username: String) {
        Thread {
            val error = NativeLibrary.nextendoSetUsername(username)
            post {
                Toast.makeText(
                    CitronApplication.appContext,
                    if (error.isEmpty()) {
                        R.string.nextendo_username_updated
                    } else {
                        R.string.nextendo_username_failed
                    },
                    Toast.LENGTH_LONG
                ).show()
                if (error.isEmpty()) {
                    NextendoAccountState.refresh()
                }
            }
        }.start()
    }

    // The database write happens with no guest running; a running game holds its own copy of
    // the Mii database in memory and would overwrite it on exit.
    private fun showMiiDialog() {
        if (NativeLibrary.isRunning()) {
            Toast.makeText(
                CitronApplication.appContext,
                R.string.nextendo_mii_stop_game,
                Toast.LENGTH_LONG
            ).show()
            return
        }

        val miiBase64 = NextendoAccountState.profile?.miiBase64.orEmpty()
        val actions = mutableListOf<Pair<Int, () -> Unit>>()
        if (miiBase64.isNotEmpty()) {
            actions += R.string.nextendo_mii_apply to { applyAccountMii(miiBase64) }
            actions += R.string.nextendo_mii_remove to { removeAccountMii(miiBase64) }
        }
        actions += R.string.nextendo_mii_create to { createAccountMii() }

        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_mii)
            .setItems(actions.map { getString(it.first) }.toTypedArray()) { _, which ->
                actions[which].second()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun accountMiiBytes(base64: String): ByteArray? = try {
        Base64.decode(base64, Base64.DEFAULT)
    } catch (_: IllegalArgumentException) {
        null
    }

    private fun toastMii(@StringRes message: Int) {
        Toast.makeText(CitronApplication.appContext, message, Toast.LENGTH_LONG).show()
    }

    private fun applyAccountMii(base64: String) {
        val bytes = accountMiiBytes(base64)
        if (bytes == null) {
            toastMii(R.string.nextendo_mii_invalid)
            return
        }
        Thread {
            val message = when (NativeLibrary.nextendoMiiApply(bytes)) {
                "applied" -> R.string.nextendo_mii_applied
                "invalid" -> R.string.nextendo_mii_invalid
                else -> R.string.nextendo_mii_failed
            }
            post { toastMii(message) }
        }.start()
    }

    private fun removeAccountMii(base64: String) {
        val bytes = accountMiiBytes(base64)
        if (bytes == null) {
            toastMii(R.string.nextendo_mii_invalid)
            return
        }
        Thread {
            val message = when (NativeLibrary.nextendoMiiRemove(bytes)) {
                "removed" -> R.string.nextendo_mii_removed
                "not_found" -> R.string.nextendo_mii_not_found
                "invalid" -> R.string.nextendo_mii_invalid
                else -> R.string.nextendo_mii_failed
            }
            post { toastMii(message) }
        }.start()
    }

    private fun createAccountMii() {
        Thread {
            val bytes = NativeLibrary.nextendoMiiCreate()
            if (bytes == null) {
                post { toastMii(R.string.nextendo_mii_failed) }
                return@Thread
            }
            val base64 = Base64.encodeToString(bytes, Base64.NO_WRAP)
            if (NativeLibrary.nextendoPushProfileMii(base64).isNotEmpty()) {
                post { toastMii(R.string.nextendo_mii_failed) }
                return@Thread
            }
            val message = if (NativeLibrary.nextendoMiiApply(bytes) == "applied") {
                R.string.nextendo_mii_created
            } else {
                R.string.nextendo_mii_uploaded_only
            }
            post {
                toastMii(message)
                NextendoAccountState.refresh()
            }
        }.start()
    }

    // Account actions the website covers that this client doesn't (email, password, friend
    // requests from the web, ...).
    private fun openWebsiteProfile() {
        val url = NativeLibrary.nextendoWebsiteProfileUrl()
        if (url.isEmpty()) {
            return
        }
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (_: ActivityNotFoundException) {
        }
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

    companion object {
        const val TAG = "NextendoProfileDialogFragment"
        private val USERNAME_PATTERN = Regex("^[A-Za-z0-9_-]{3,16}$")
    }
}
