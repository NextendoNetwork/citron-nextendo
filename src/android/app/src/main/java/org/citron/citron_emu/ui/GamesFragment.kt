// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.ui

import android.content.SharedPreferences
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.preference.PreferenceManager
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.adapters.GameAdapter
import org.citron.citron_emu.databinding.FragmentGamesBinding
import org.citron.citron_emu.layout.AutofitGridLayoutManager
import org.citron.citron_emu.model.GamesViewModel
import org.citron.citron_emu.model.HomeViewModel
import org.citron.citron_emu.utils.Log
import org.citron.citron_emu.utils.ViewUtils.setVisible
import org.citron.citron_emu.utils.ViewUtils.updateMargins
import org.citron.citron_emu.utils.collect

class GamesFragment : Fragment() {
    private var _binding: FragmentGamesBinding? = null
    private val binding get() = _binding!!

    private val gamesViewModel: GamesViewModel by activityViewModels()
    private val homeViewModel: HomeViewModel by activityViewModels()

    private lateinit var gameAdapter: GameAdapter
    private lateinit var preferences: SharedPreferences
    private var viewMode = VIEW_MODE_LIST

    private val onlineCountsHandler = Handler(Looper.getMainLooper())
    @Volatile private var onlineCountsPolling = false
    private val onlineCountsPoll = object : Runnable {
        override fun run() {
            Thread {
                try {
                    val json = NativeLibrary.nextendoOnlineCountsJson()
                    if (json.isNotEmpty() && _binding != null && ::gameAdapter.isInitialized) {
                        val counts = HashMap<String, Int>()
                        val obj = Json.parseToJsonElement(json).jsonObject
                        for ((key, value) in obj) {
                            counts[key] = value.jsonPrimitive.int
                        }
                        // An empty answer means the fetch failed; keep the last known counts.
                        if (counts.isNotEmpty()) {
                            Handler(Looper.getMainLooper()).post {
                                gameAdapter.setOnlineCounts(counts)
                            }
                        }
                    }
                } catch (e: Exception) {
                    Log.error("[GamesFragment] online counts poll failed: ${e.message}")
                }
                if (onlineCountsPolling) {
                    onlineCountsHandler.postDelayed(this, ONLINE_COUNTS_POLL_MS)
                }
            }.start()
        }
    }

    companion object {
        private const val PREF_VIEW_MODE = "pref_games_view_mode"
        private const val VIEW_MODE_LIST = 0
        private const val VIEW_MODE_GRID = 1
        private const val VIEW_MODE_COMPACT_GRID = 2
        private const val VIEW_MODE_COUNT = 3
        private const val ONLINE_COUNTS_POLL_MS = 30_000L
    }

    override fun onResume() {
        super.onResume()
        onlineCountsPolling = true
        onlineCountsHandler.removeCallbacks(onlineCountsPoll)
        onlineCountsHandler.post(onlineCountsPoll)
    }

    override fun onPause() {
        super.onPause()
        onlineCountsPolling = false
        onlineCountsHandler.removeCallbacks(onlineCountsPoll)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentGamesBinding.inflate(inflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setNavigationVisibility(visible = true, animated = true)
        homeViewModel.setStatusBarShadeVisibility(true)

        preferences = PreferenceManager.getDefaultSharedPreferences(requireContext())
        viewMode = preferences.getInt(PREF_VIEW_MODE, VIEW_MODE_LIST)

        gameAdapter = GameAdapter(requireActivity() as AppCompatActivity, viewMode != VIEW_MODE_LIST)

        binding.gridGames.apply {
            layoutManager = layoutManagerForMode(viewMode)
            adapter = gameAdapter
        }

        binding.btnViewToggle.apply {
            setOnClickListener { toggleViewMode() }
        }
        updateToggleButton()

        binding.swipeRefresh.apply {
            setOnRefreshListener {
                // Use the progress bar for the potentially long-running game scan.
                isRefreshing = false
                gamesViewModel.reloadGames(false)
            }
        }

        gamesViewModel.isReloading.collect(viewLifecycleOwner) {
            binding.scanProgress.setVisible(it)
            binding.noticeText.setVisible(
                visible = gamesViewModel.games.value.isEmpty() && !it,
                gone = false
            )
        }
        gamesViewModel.games.collect(viewLifecycleOwner) {
            gameAdapter.submitList(it)
        }
        gamesViewModel.shouldSwapData.collect(
            viewLifecycleOwner,
            resetState = { gamesViewModel.setShouldSwapData(false) }
        ) {
            if (it) {
                gameAdapter.submitList(gamesViewModel.games.value)
            }
        }
        gamesViewModel.shouldScrollToTop.collect(
            viewLifecycleOwner,
            resetState = { gamesViewModel.setShouldScrollToTop(false) }
        ) { if (it) scrollToTop() }

        setInsets()
    }
    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun layoutManagerForMode(mode: Int): RecyclerView.LayoutManager =
        when (mode) {
            VIEW_MODE_GRID -> AutofitGridLayoutManager(
                requireContext(),
                resources.getDimensionPixelSize(R.dimen.card_width)
            )
            VIEW_MODE_COMPACT_GRID -> AutofitGridLayoutManager(
                requireContext(),
                resources.getDimensionPixelSize(R.dimen.card_width_small)
            )
            else -> LinearLayoutManager(requireContext())
        }

    private fun updateToggleButton() {
        val (iconRes, descRes) = when (viewMode) {
            VIEW_MODE_LIST -> Pair(R.drawable.ic_view_grid, R.string.switch_to_grid_view)
            VIEW_MODE_GRID -> Pair(
                R.drawable.ic_view_grid_3,
                R.string.switch_to_compact_grid_view
            )
            else -> Pair(R.drawable.ic_view_list, R.string.switch_to_list_view)
        }
        binding.btnViewToggle.setIconResource(iconRes)
        binding.btnViewToggle.contentDescription = getString(descRes)
    }

    private fun toggleViewMode() {
        val previousViewMode = viewMode
        viewMode = (viewMode + 1) % VIEW_MODE_COUNT
        preferences.edit().putInt(PREF_VIEW_MODE, viewMode).apply()

        binding.gridGames.layoutManager = layoutManagerForMode(viewMode)
        gameAdapter.setTilesMode(viewMode != VIEW_MODE_LIST)
        // Grid modes share a view type, so rebind items at the new size.
        if (previousViewMode != VIEW_MODE_LIST && viewMode != VIEW_MODE_LIST) {
            gameAdapter.notifyDataSetChanged()
        }
        updateToggleButton()
        ViewCompat.requestApplyInsets(binding.root)
    }

    private fun scrollToTop() {
        if (_binding != null) {
            binding.gridGames.smoothScrollToPosition(0)
        }
    }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(
            binding.root
        ) { view: View, windowInsets: WindowInsetsCompat ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())
            val extraListSpacing = resources.getDimensionPixelSize(R.dimen.spacing_large)
            val spacingNavigation = resources.getDimensionPixelSize(R.dimen.spacing_navigation)
            val spacingNavigationRail =
                resources.getDimensionPixelSize(R.dimen.spacing_navigation_rail)

            binding.gridGames.updatePadding(
                top = barInsets.top + extraListSpacing,
                bottom = barInsets.bottom + spacingNavigation + extraListSpacing
            )

            binding.swipeRefresh.setProgressViewEndTarget(
                false,
                barInsets.top + resources.getDimensionPixelSize(R.dimen.spacing_refresh_end)
            )

            val leftInsets = barInsets.left + cutoutInsets.left
            val rightInsets = barInsets.right + cutoutInsets.right
            val left = leftInsets +
                if (view.layoutDirection == View.LAYOUT_DIRECTION_LTR) spacingNavigationRail else 0
            val right = rightInsets +
                if (view.layoutDirection == View.LAYOUT_DIRECTION_RTL) spacingNavigationRail else 0
            binding.swipeRefresh.updateMargins(left = left, right = right)

            binding.scanProgress.updateMargins(
                left = left,
                top = barInsets.top,
                right = right
            )

            binding.noticeText.updatePadding(bottom = spacingNavigation)

            val toggleSpacing = resources.getDimensionPixelSize(R.dimen.spacing_med)
            binding.btnViewToggle.updateMargins(
                left = leftInsets + toggleSpacing,
                top = barInsets.top + toggleSpacing,
                right = rightInsets + toggleSpacing
            )

            windowInsets
        }
}
