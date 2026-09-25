// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.app.Dialog
import android.os.Bundle
import android.view.View
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.adapters.NextendoHistoryAdapter
import org.citron.citron_emu.databinding.DialogNextendoPlayHistoryBinding
import org.json.JSONObject

class NextendoPlayHistoryDialogFragment : NextendoDialogFragment<DialogNextendoPlayHistoryBinding>() {
    private val adapter = NextendoHistoryAdapter()

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        inflateBinding { DialogNextendoPlayHistoryBinding.inflate(it) }
        binding.listHistory.layoutManager = LinearLayoutManager(requireContext())
        binding.listHistory.adapter = adapter
        binding.textHistoryStatus.setText(R.string.nextendo_play_history_loading)
        loadHistory()

        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_play_history)
            .setView(binding.root)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
    }

    private fun loadHistory() {
        Thread {
            val json = NativeLibrary.nextendoGetHistoryJson()
            val items = mutableListOf<NextendoHistoryAdapter.Item>()
            var ok = false
            try {
                val root = JSONObject(json)
                ok = root.optBoolean("ok")
                val entries = root.getJSONArray("entries")
                for (i in 0 until entries.length()) {
                    val entry = entries.getJSONObject(i)
                    items.add(
                        NextendoHistoryAdapter.Item(
                            name = entry.getString("name"),
                            iconBase64 = entry.getString("icon"),
                            seconds = entry.getLong("seconds"),
                            lastPlayed = entry.getString("last_played")
                        )
                    )
                }
            } catch (_: Exception) {
            }
            post {
                adapter.submit(items)
                when {
                    !ok -> {
                        binding.textHistoryStatus.setText(R.string.nextendo_play_history_error)
                        binding.textHistoryStatus.visibility = View.VISIBLE
                    }

                    items.isEmpty() -> {
                        binding.textHistoryStatus.setText(R.string.nextendo_play_history_empty)
                        binding.textHistoryStatus.visibility = View.VISIBLE
                    }

                    else -> binding.textHistoryStatus.visibility = View.GONE
                }
            }
        }.start()
    }

    companion object {
        const val TAG = "NextendoPlayHistoryDialogFragment"
    }
}
