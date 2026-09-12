// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.adapters

import android.graphics.Bitmap
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.core.view.isVisible
import androidx.recyclerview.widget.RecyclerView
import org.citron.citron_emu.R
import org.citron.citron_emu.databinding.ListItemFriendBinding
import org.citron.citron_emu.utils.NextendoImages

class NextendoFriendAdapter : RecyclerView.Adapter<NextendoFriendAdapter.ViewHolder>() {

    class Item(
        val pid: Long,
        val name: String,
        val status: Int,
        val appName: String,
        val imageBase64: String,
        val request: Boolean
    )

    val items = mutableListOf<Item>()

    var onAccept: ((Item) -> Unit)? = null
    var onDecline: ((Item) -> Unit)? = null
    var onRemove: ((Item) -> Unit)? = null

    private val avatars = HashMap<Long, Bitmap?>()

    fun submit(newItems: List<Item>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        return ViewHolder(
            ListItemFriendBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        )
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        holder.bind(items[position])
    }

    inner class ViewHolder(private val binding: ListItemFriendBinding) :
        RecyclerView.ViewHolder(binding.root) {

        fun bind(item: Item) {
            val context = binding.root.context
            binding.textFriendName.text = item.name
            binding.textFriendStatus.text = when {
                item.status <= 0 -> context.getString(R.string.nextendo_friend_status_offline)
                item.appName.isNotEmpty() -> context.getString(
                    R.string.nextendo_friend_status_playing,
                    item.appName
                )

                else -> context.getString(R.string.nextendo_friend_status_online)
            }

            val avatar = avatars.getOrPut(item.pid) {
                NextendoImages.decode(item.imageBase64)
            }
            binding.imageAvatar.setImageBitmap(avatar)
            binding.imageAvatar.isVisible = avatar != null

            binding.buttonAccept.isVisible = item.request
            binding.buttonDecline.isVisible = item.request
            binding.buttonRemove.isVisible = !item.request
            binding.buttonAccept.setOnClickListener { onAccept?.invoke(item) }
            binding.buttonDecline.setOnClickListener { onDecline?.invoke(item) }
            binding.buttonRemove.setOnClickListener { onRemove?.invoke(item) }
        }
    }
}
