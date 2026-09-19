// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.app.Dialog
import android.os.Bundle
import android.widget.Toast
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.adapters.NextendoCloudSaveAdapter
import org.citron.citron_emu.databinding.DialogNextendoCloudSavesBinding
import org.citron.citron_emu.utils.GameHelper
import org.citron.citron_emu.utils.NextendoCloudSaveResult

class NextendoCloudSavesDialogFragment : NextendoDialogFragment<DialogNextendoCloudSavesBinding>() {
    private val adapter = NextendoCloudSaveAdapter()

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        inflateBinding { DialogNextendoCloudSavesBinding.inflate(it) }
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

    private fun loadGames() {
        Thread {
            val blocked = NativeLibrary.isRunning()
            val loaded = GameHelper.cachedGames.ifEmpty { GameHelper.getGames() }
            val games = loaded.filter {
                NativeLibrary.isNextendoCloudSaveTitle(it.programId.toLongOrNull() ?: 0L)
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
            NextendoCloudSaveResult.pull(
                NativeLibrary.nextendoCloudSavePull(item.programId, force = true)
            )
        }
    }

    private fun upload(item: NextendoCloudSaveAdapter.Item) {
        runAction(item) {
            NextendoCloudSaveResult.push(
                NativeLibrary.nextendoCloudSavePush(item.programId, manual = true)
            )
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

    companion object {
        const val TAG = "NextendoCloudSavesDialogFragment"
    }
}
