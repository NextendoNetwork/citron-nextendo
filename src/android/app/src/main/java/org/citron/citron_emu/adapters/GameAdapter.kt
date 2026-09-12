// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.adapters

import android.net.Uri
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.navigation.findNavController
import androidx.preference.PreferenceManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.HomeNavigationDirections
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.databinding.CardGameCoverBinding
import org.citron.citron_emu.databinding.CardGameListBinding
import org.citron.citron_emu.model.Game
import org.citron.citron_emu.model.GamesViewModel
import org.citron.citron_emu.utils.GameIconUtils
import org.citron.citron_emu.viewholder.AbstractViewHolder

class GameAdapter(private val activity: AppCompatActivity, private var tilesMode: Boolean = false) :
    AbstractDiffAdapter<Game, AbstractViewHolder<Game>>(exact = false) {

    companion object {
        private const val VIEW_TYPE_LIST = 0
        private const val VIEW_TYPE_TILES = 1
    }

    private var onlineCounts: HashMap<String, Int> = HashMap()
    private var showNextendoPills = true

    fun setOnlineCounts(counts: HashMap<String, Int>) {
        onlineCounts = counts
        notifyDataSetChanged()
    }

    // Off in search results, where the pills overlap the game title.
    fun setShowNextendoPills(enabled: Boolean) {
        if (showNextendoPills != enabled) {
            showNextendoPills = enabled
            notifyDataSetChanged()
        }
    }

    private fun bindPills(onlinePill: TextView, updatePill: TextView, model: Game) {
        if (!showNextendoPills) {
            onlinePill.visibility = View.GONE
            updatePill.visibility = View.GONE
            return
        }
        val programIdLong = model.programId.toLongOrNull() ?: 0L
        val linked = NativeLibrary.getNextendoAccountStatus().isNotEmpty()
        val nextendoTitle = NativeLibrary.isNextendoTitle(programIdLong)
        val count = onlineCounts[programIdLong.toString(16).padStart(16, '0')]
        val isOnlinePillVisible = linked && nextendoTitle && count != null
        onlinePill.visibility = if (isOnlinePillVisible) View.VISIBLE else View.GONE
        if (count != null) {
            onlinePill.text = onlinePill.context.getString(R.string.nextendo_online_pill, count)
        }

        val requiredVersion = NativeLibrary.nextendoRequiredVersion(programIdLong)
        val needsUpdate = linked && nextendoTitle &&
            model.version.isNotEmpty() && model.version != requiredVersion
        updatePill.visibility = if (needsUpdate) View.VISIBLE else View.GONE
        if (needsUpdate) {
            updatePill.text = updatePill.context.getString(R.string.nextendo_update_pill, requiredVersion)
        }
    }

    fun setTilesMode(enabled: Boolean) {
        if (tilesMode != enabled) {
            tilesMode = enabled
            notifyDataSetChanged()
        }
    }

    override fun getItemViewType(position: Int): Int =
        if (tilesMode) VIEW_TYPE_TILES else VIEW_TYPE_LIST

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): AbstractViewHolder<Game> {
        return when (viewType) {
            VIEW_TYPE_TILES -> {
                val binding = CardGameCoverBinding.inflate(
                    LayoutInflater.from(parent.context), parent, false
                )
                GameTilesViewHolder(binding)
            }
            else -> {
                val binding = CardGameListBinding.inflate(
                    LayoutInflater.from(parent.context), parent, false
                )
                GameListViewHolder(binding)
            }
        }
    }

    inner class GameListViewHolder(val binding: CardGameListBinding) :
        AbstractViewHolder<Game>(binding) {
        override fun bind(model: Game) {
            binding.imageGameScreen.scaleType = ImageView.ScaleType.CENTER_CROP
            GameIconUtils.loadGameIcon(model, binding.imageGameScreen)

            binding.textGameTitle.text = model.title.replace("[\\t\\n\\r]+".toRegex(), " ")

            bindPills(binding.textOnlinePill, binding.textUpdatePill, model)

            binding.cardGame.setOnClickListener { onClick(model) }
            binding.cardGame.setOnLongClickListener { onLongClick(model) }
        }

        fun onClick(game: Game) {
            handleGameClick(game)
        }

        fun onLongClick(game: Game): Boolean {
            return handleGameLongClick(game)
        }
    }

    inner class GameTilesViewHolder(val binding: CardGameCoverBinding) :
        AbstractViewHolder<Game>(binding) {
        override fun bind(model: Game) {
            binding.imageGameScreen.scaleType = ImageView.ScaleType.CENTER_CROP
            GameIconUtils.loadGameIcon(model, binding.imageGameScreen)

            bindPills(binding.textOnlinePill, binding.textUpdatePill, model)

            binding.cardGame.setOnClickListener { handleGameClick(model) }
            binding.cardGame.setOnLongClickListener { handleGameLongClick(model) }
        }
    }

    private fun handleGameClick(game: Game) {
        val gameExists = DocumentFile.fromSingleUri(
            CitronApplication.appContext,
            Uri.parse(game.path)
        )?.exists() == true
        if (!gameExists) {
            Toast.makeText(
                CitronApplication.appContext,
                R.string.loader_error_file_not_found,
                Toast.LENGTH_LONG
            ).show()

            ViewModelProvider(activity)[GamesViewModel::class.java].reloadGames(true)
            return
        }

        val preferences =
            PreferenceManager.getDefaultSharedPreferences(CitronApplication.appContext)
        preferences.edit()
            .putLong(
                game.keyLastPlayedTime,
                System.currentTimeMillis()
            )
            .apply()

        activity.lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                val shortcut =
                    ShortcutInfoCompat.Builder(CitronApplication.appContext, game.path)
                        .setShortLabel(game.title)
                        .setIcon(GameIconUtils.getShortcutIcon(activity, game))
                        .setIntent(game.launchIntent)
                        .build()
                ShortcutManagerCompat.pushDynamicShortcut(CitronApplication.appContext, shortcut)
            }
        }

        val action = HomeNavigationDirections.actionGlobalEmulationActivity(game, true)
        activity.findNavController(R.id.fragment_container).navigate(action)
    }

    private fun handleGameLongClick(game: Game): Boolean {
        val action = HomeNavigationDirections.actionGlobalPerGamePropertiesFragment(game)
        activity.findNavController(R.id.fragment_container).navigate(action)
        return true
    }
}
