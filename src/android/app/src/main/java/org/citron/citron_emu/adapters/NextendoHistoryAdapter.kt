// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.adapters

import android.graphics.Bitmap
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import org.citron.citron_emu.R
import org.citron.citron_emu.databinding.ListItemHistoryBinding
import org.citron.citron_emu.utils.NextendoImages
import java.time.OffsetDateTime
import java.time.format.DateTimeFormatter

class NextendoHistoryAdapter : RecyclerView.Adapter<NextendoHistoryAdapter.ViewHolder>() {

    class Item(
        val name: String,
        val iconBase64: String,
        val seconds: Long,
        val lastPlayed: String
    )

    val items = mutableListOf<Item>()

    private val icons = HashMap<String, Bitmap?>()
    private val lastPlayedFormat = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm")

    fun submit(newItems: List<Item>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        return ViewHolder(
            ListItemHistoryBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        )
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        holder.bind(items[position])
    }

    inner class ViewHolder(private val binding: ListItemHistoryBinding) :
        RecyclerView.ViewHolder(binding.root) {

        fun bind(item: Item) {
            binding.textHistoryName.text = item.name
            binding.textHistoryDetails.text = binding.root.context.getString(
                R.string.nextendo_history_details,
                formatPlayTime(item.seconds),
                formatLastPlayed(item.lastPlayed)
            )

            val icon = icons.getOrPut(item.iconBase64) {
                NextendoImages.decode(item.iconBase64)
            }
            binding.imageHistoryIcon.setImageBitmap(icon)
        }
    }

    private fun formatPlayTime(seconds: Long): String {
        val hours = seconds / 3600
        val minutes = (seconds % 3600) / 60
        return when {
            hours > 0 -> "${hours}h ${minutes}m"
            minutes > 0 -> "${minutes}m"
            else -> "${seconds}s"
        }
    }

    private fun formatLastPlayed(value: String): String = if (value.isEmpty()) {
        ""
    } else {
        try {
            OffsetDateTime.parse(value).format(lastPlayedFormat)
        } catch (_: Exception) {
            value
        }
    }
}
