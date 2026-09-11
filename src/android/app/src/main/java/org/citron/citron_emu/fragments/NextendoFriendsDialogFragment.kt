// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.app.Dialog
import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.DialogFragment
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.adapters.NextendoFriendAdapter
import org.citron.citron_emu.databinding.DialogNextendoAddFriendBinding
import org.citron.citron_emu.databinding.DialogNextendoFriendsBinding
import org.json.JSONArray
import org.json.JSONObject

class NextendoFriendsDialogFragment : DialogFragment() {
    private var _binding: DialogNextendoFriendsBinding? = null
    private val binding get() = _binding!!

    private val adapter = NextendoFriendAdapter()

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        _binding = DialogNextendoFriendsBinding.inflate(layoutInflater)
        adapter.onAccept = { act { NativeLibrary.nextendoAcceptFriend(it.pid) } }
        adapter.onDecline = { act { NativeLibrary.nextendoDeclineFriend(it.pid) } }
        adapter.onRemove = { act { NativeLibrary.nextendoRemoveFriend(it.pid) } }
        binding.listFriends.layoutManager = LinearLayoutManager(requireContext())
        binding.listFriends.adapter = adapter
        binding.textFriendsStatus.setText(R.string.nextendo_friends_loading)
        refresh()

        val dialog = MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_friends)
            .setView(binding.root)
            .setPositiveButton(R.string.nextendo_friend_add, null)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener { showAddFriend() }
        }
        return dialog
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun refresh() {
        Thread {
            val json = NativeLibrary.nextendoFriendsListJson()
            val items = mutableListOf<NextendoFriendAdapter.Item>()
            var ok = false
            try {
                val root = JSONObject(json)
                ok = root.optBoolean("ok")
                addItems(items, root.getJSONArray("requests"), true)
                addItems(items, root.getJSONArray("friends"), false)
            } catch (_: Exception) {
            }
            post {
                adapter.submit(items)
                when {
                    !ok -> {
                        binding.textFriendsStatus.setText(R.string.nextendo_friends_error)
                        binding.textFriendsStatus.visibility = View.VISIBLE
                    }

                    items.isEmpty() -> {
                        binding.textFriendsStatus.setText(R.string.nextendo_friends_empty)
                        binding.textFriendsStatus.visibility = View.VISIBLE
                    }

                    else -> binding.textFriendsStatus.visibility = View.GONE
                }
            }
        }.start()
    }

    private fun addItems(
        out: MutableList<NextendoFriendAdapter.Item>,
        array: JSONArray,
        request: Boolean
    ) {
        for (i in 0 until array.length()) {
            val entry = array.getJSONObject(i)
            out.add(
                NextendoFriendAdapter.Item(
                    pid = entry.getLong("pid"),
                    name = entry.getString("name"),
                    status = entry.getInt("status"),
                    appName = entry.getString("app_name"),
                    imageBase64 = entry.getString("image"),
                    request = request
                )
            )
        }
    }

    private fun act(action: () -> String) {
        Thread {
            val error = action()
            post {
                Toast.makeText(
                    CitronApplication.appContext,
                    if (error.isEmpty()) R.string.nextendo_friend_updated else R.string.nextendo_friend_failed,
                    Toast.LENGTH_SHORT
                ).show()
                refresh()
            }
        }.start()
    }

    private fun showAddFriend() {
        val dialogBinding = DialogNextendoAddFriendBinding.inflate(layoutInflater)
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_friend_add)
            .setView(dialogBinding.root)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                val code = dialogBinding.editFriendCode.text?.toString()?.trim().orEmpty()
                if (code.isNotEmpty()) {
                    act { NativeLibrary.nextendoAddFriend(code) }
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
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
        const val TAG = "NextendoFriendsDialogFragment"
    }
}
