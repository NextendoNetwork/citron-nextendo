// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.app.Dialog
import android.os.Bundle
import android.widget.Toast
import androidx.fragment.app.DialogFragment
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.adapters.NextendoCloudSaveAdapter
import org.citron.citron_emu.databinding.DialogNextendoCloudSavesBinding
import org.citron.citron_emu.utils.GameHelper

class NextendoCloudSavesDialogFragment : DialogFragment() {
    private var _binding: DialogNextendoCloudSavesBinding? = null
    private val binding get() = _binding!!

    private val adapter = NextendoCloudSaveAdapter()

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        _binding = DialogNextendoCloudSavesBinding.inflate(layoutInflater)
        adapter.onDownload = { confirmDownload(it) }
        adapter.onUpload = { upload(it) }
        binding.listCloudSaves.layoutManager = LinearLayoutManager(requireContext())
        binding.listCloudSaves.adapter = adapter
        binding.textCloudStatus.setText(R.string.nextendo_cloud_save_loading)
        loadGames()

        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_manage_cloud_saves)
            .setView(binding.root)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun loadGames() {
        Thread {
            val blocked = NativeLibrary.isRunning()
            val loaded = GameHelper.cachedGames.ifEmpty { GameHelper.getGames() }
            val games = loaded.filter {
                NativeLibrary.isNextendoTitle(it.programId.toLongOrNull() ?: 0L)
            }
            post {
                adapter.submit(games.map {
                    NextendoCloudSaveAdapter.Item(it.title, it.programId.toLong(), blocked)
                })
                binding.textCloudStatus.setText(
                    when {
                        games.isEmpty() -> R.string.nextendo_cloud_save_no_games
                        blocked -> R.string.nextendo_cloud_save_stop_game
                        else -> R.string.nextendo_cloud_save_checking
                    }
                )
                if (!blocked && games.isNotEmpty()) {
                    probeAll()
                }
            }
        }.start()
    }

    private fun probeAll() {
        Thread {
            for (index in adapter.items.indices) {
                val item = adapter.items[index]
                val available = NativeLibrary.nextendoCloudSaveProbe(item.programId) == "available"
                post {
                    item.status = getString(
                        if (available) {
                            R.string.nextendo_cloud_save_available
                        } else {
                            R.string.nextendo_cloud_save_none
                        }
                    )
                    adapter.notifyItemChanged(index)
                }
            }
            post { binding.textCloudStatus.text = "" }
        }.start()
    }

    private fun confirmDownload(item: NextendoCloudSaveAdapter.Item) {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.nextendo_cloud_save_download)
            .setMessage(getString(R.string.nextendo_cloud_save_confirm, item.name))
            .setPositiveButton(android.R.string.ok) { _, _ -> download(item) }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun download(item: NextendoCloudSaveAdapter.Item) {
        runAction(item) {
            when (NativeLibrary.nextendoCloudSavePull(item.programId, force = true)) {
                "applied" -> R.string.nextendo_cloud_save_applied
                "kept" -> R.string.nextendo_cloud_save_kept_local
                "none" -> R.string.nextendo_cloud_save_none
                "disabled" -> R.string.nextendo_cloud_save_disabled
                else -> R.string.nextendo_cloud_save_failed
            }
        }
    }

    private fun upload(item: NextendoCloudSaveAdapter.Item) {
        runAction(item) {
            when (NativeLibrary.nextendoCloudSavePush(item.programId, manual = true)) {
                "uploaded" -> R.string.nextendo_cloud_save_uploaded
                "none" -> R.string.nextendo_cloud_save_none
                "disabled" -> R.string.nextendo_cloud_save_disabled
                else -> R.string.nextendo_cloud_save_failed
            }
        }
    }

    private fun runAction(item: NextendoCloudSaveAdapter.Item, action: () -> Int) {
        item.busy = true
        adapter.notifyItemChanged(adapter.items.indexOf(item))
        Thread {
            val message = action()
            post {
                item.busy = false
                adapter.notifyItemChanged(adapter.items.indexOf(item))
                Toast.makeText(CitronApplication.appContext, message, Toast.LENGTH_LONG).show()
            }
        }.start()
    }

    private fun post(block: () -> Unit) {
        activity?.runOnUiThread {
            if (isAdded && _binding != null) {
                block()
            }
        }
    }

    companion object {
        const val TAG = "NextendoCloudSavesDialogFragment"
    }
}
