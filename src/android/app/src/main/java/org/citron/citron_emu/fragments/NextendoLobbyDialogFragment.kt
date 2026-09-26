// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.app.Dialog
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Toast
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.adapters.NextendoLobbyPlayerAdapter
import org.citron.citron_emu.databinding.DialogNextendoLobbyBinding
import org.json.JSONArray
import org.json.JSONObject

// In-game lobby menu: the lobby the account is in right now (state, counts, members) and the
// recent-players list, mirroring the desktop client's lobby tab. Polls while shown so the
// searching/matched state stays live during matchmaking.
class NextendoLobbyDialogFragment : NextendoDialogFragment<DialogNextendoLobbyBinding>() {
    private val memberAdapter = NextendoLobbyPlayerAdapter()
    private val recentAdapter = NextendoLobbyPlayerAdapter()

    private val handler = Handler(Looper.getMainLooper())

    @Volatile
    private var refreshing = false

    private val refreshTick = object : Runnable {
        override fun run() {
            refresh()
            handler.postDelayed(this, REFRESH_INTERVAL_MS)
        }
    }

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        inflateBinding { DialogNextendoLobbyBinding.inflate(it) }
        memberAdapter.onAdd = { addFriend(it) }
        recentAdapter.onAdd = { addFriend(it) }
        binding.listLobby.layoutManager = LinearLayoutManager(requireContext())
        binding.listLobby.adapter = memberAdapter
        binding.listRecent.layoutManager = LinearLayoutManager(requireContext())
        binding.listRecent.adapter = recentAdapter
        binding.textLobbyStatus.setText(R.string.nextendo_friends_loading)
        binding.textMembersEmpty.visibility = View.GONE
        binding.textRecentHeader.visibility = View.GONE

        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_lobby)
            .setView(binding.root)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
    }

    override fun onResume() {
        super.onResume()
        handler.removeCallbacks(refreshTick)
        refreshTick.run()
    }

    override fun onPause() {
        handler.removeCallbacks(refreshTick)
        super.onPause()
    }

    private fun refresh() {
        if (refreshing) {
            return
        }
        if (NativeLibrary.getNextendoAccountStatus().isEmpty()) {
            binding.textLobbyStatus.setText(R.string.nextendo_lobby_signed_out)
            memberAdapter.submit(emptyList())
            recentAdapter.submit(emptyList())
            binding.textMembersEmpty.visibility = View.GONE
            binding.textRecentHeader.visibility = View.GONE
            return
        }

        refreshing = true
        Thread {
            val lobbyJson = NativeLibrary.nextendoGetLobbyJson()
            val recentJson = NativeLibrary.nextendoGetRecentPlayersJson()

            var inLobby = false
            var stateCode = ""
            var count = 0
            var max = 0
            val members = mutableListOf<NextendoLobbyPlayerAdapter.Item>()
            val recent = mutableListOf<NextendoLobbyPlayerAdapter.Item>()
            try {
                val root = JSONObject(lobbyJson)
                inLobby = root.optBoolean("in_lobby")
                stateCode = root.optString("state_code")
                count = root.optInt("count")
                max = root.optInt("max")
                addPlayers(members, root.optJSONArray("players"))
                addPlayers(recent, JSONObject(recentJson).optJSONArray("players"))
            } catch (_: Exception) {
            }

            post {
                binding.textLobbyStatus.text = when {
                    !inLobby -> getString(R.string.nextendo_lobby_not_in)
                    stateCode == "searching" -> getString(
                        R.string.nextendo_lobby_players_searching,
                        count,
                        max
                    )

                    stateCode == "matched" -> getString(
                        R.string.nextendo_lobby_players_matched,
                        count,
                        max
                    )

                    else -> getString(R.string.nextendo_lobby_players, count, max)
                }
                memberAdapter.submit(members)
                recentAdapter.submit(recent)
                binding.textMembersEmpty.visibility =
                    if (inLobby && members.isEmpty()) View.VISIBLE else View.GONE
                binding.textRecentHeader.visibility =
                    if (recent.isEmpty()) View.GONE else View.VISIBLE
            }
            refreshing = false
        }.start()
    }

    private fun addPlayers(
        out: MutableList<NextendoLobbyPlayerAdapter.Item>,
        array: JSONArray?
    ) {
        if (array == null) {
            return
        }
        for (i in 0 until array.length()) {
            val entry = array.getJSONObject(i)
            val pid = entry.optLong("pid")
            val isMe = entry.optBoolean("is_me")
            val host = entry.optBoolean("host")
            val friendCode = entry.optString("friend_code")
            val known = entry.optBoolean("known")
            val name = entry.optString("name").ifEmpty { "#$pid" }

            val subtitle = mutableListOf<String>()
            if (isMe) {
                subtitle.add(getString(R.string.nextendo_lobby_me))
            } else {
                if (host) {
                    subtitle.add(getString(R.string.nextendo_lobby_host))
                }
                if (friendCode.isNotEmpty()) {
                    subtitle.add(friendCode)
                }
            }

            out.add(
                NextendoLobbyPlayerAdapter.Item(
                    pid = pid,
                    name = name,
                    subtitle = subtitle.joinToString(" · "),
                    imageBase64 = entry.optString("image"),
                    canAdd = known && !isMe && friendCode.isNotEmpty(),
                    friendCode = friendCode
                )
            )
        }
    }

    private fun addFriend(item: NextendoLobbyPlayerAdapter.Item) {
        if (item.friendCode.isEmpty()) {
            return
        }
        Thread {
            val error = NativeLibrary.nextendoAddFriend(item.friendCode)
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

    companion object {
        const val TAG = "NextendoLobbyDialogFragment"
        private const val REFRESH_INTERVAL_MS = 5_000L
    }
}
