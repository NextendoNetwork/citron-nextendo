// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.adapters

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import org.citron.citron_emu.databinding.ListItemCloudSaveBinding

class NextendoCloudSaveAdapter : RecyclerView.Adapter<NextendoCloudSaveAdapter.ViewHolder>() {

    class Item(val name: String, val programId: Long, val blocked: Boolean) {
        var status: String? = null
        var busy = false
    }

    val items = mutableListOf<Item>()

    var onDownload: ((Item) -> Unit)? = null
    var onUpload: ((Item) -> Unit)? = null

    fun submit(newItems: List<Item>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        return ViewHolder(
            ListItemCloudSaveBinding.inflate(
                LayoutInflater.from(parent.context),
                parent,
                false
            )
        )
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        holder.bind(items[position])
    }

    inner class ViewHolder(private val binding: ListItemCloudSaveBinding) :
        RecyclerView.ViewHolder(binding.root) {

        fun bind(item: Item) {
            binding.textGameName.text = item.name
            binding.textSaveStatus.text = item.status
            val enabled = !item.busy && !item.blocked
            binding.buttonDownload.isEnabled = enabled
            binding.buttonUpload.isEnabled = enabled
            binding.buttonDownload.setOnClickListener { onDownload?.invoke(item) }
            binding.buttonUpload.setOnClickListener { onUpload?.invoke(item) }
        }
    }
}
