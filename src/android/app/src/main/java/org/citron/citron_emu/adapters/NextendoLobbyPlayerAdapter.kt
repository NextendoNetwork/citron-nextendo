// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.adapters

import android.graphics.Bitmap
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.core.view.isVisible
import androidx.recyclerview.widget.RecyclerView
import org.citron.citron_emu.databinding.ListItemLobbyPlayerBinding
import org.citron.citron_emu.utils.NextendoImages

class NextendoLobbyPlayerAdapter : RecyclerView.Adapter<NextendoLobbyPlayerAdapter.ViewHolder>() {

    class Item(
        val pid: Long,
        val name: String,
        val subtitle: String,
        val imageBase64: String,
        val canAdd: Boolean,
        val friendCode: String
    )

    val items = mutableListOf<Item>()

    var onAdd: ((Item) -> Unit)? = null

    private val avatars = HashMap<Long, Bitmap?>()

    fun submit(newItems: List<Item>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        return ViewHolder(
            ListItemLobbyPlayerBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        )
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        holder.bind(items[position])
    }

    inner class ViewHolder(private val binding: ListItemLobbyPlayerBinding) :
        RecyclerView.ViewHolder(binding.root) {

        fun bind(item: Item) {
            binding.textPlayerName.text = item.name
            binding.textPlayerSubtitle.text = item.subtitle
            binding.textPlayerSubtitle.isVisible = item.subtitle.isNotEmpty()

            val avatar = avatars.getOrPut(item.pid) {
                NextendoImages.decode(item.imageBase64)
            }
            binding.imageAvatar.setImageBitmap(avatar)
            binding.imageAvatar.isVisible = avatar != null

            binding.buttonAdd.isVisible = item.canAdd
            binding.buttonAdd.setOnClickListener { onAdd?.invoke(item) }
        }
    }
}
