// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.utils

import android.content.SharedPreferences
import android.net.Uri
import androidx.preference.PreferenceManager
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.model.Game
import org.citron.citron_emu.model.GameDir
import org.citron.citron_emu.model.MinimalDocumentFile

object GameHelper {
    private const val KEY_OLD_GAME_PATH = "game_path"
    const val KEY_GAMES = "Games"

    // The last full scan's result. Screens that only need the list can reuse it instead of
    // paying for another SAF walk -- the cloud save manager used to rescan and hang on large
    // libraries. The games screen populates this on every reload.
    @Volatile
    var cachedGames: List<Game> = emptyList()
        private set

    private lateinit var preferences: SharedPreferences

    fun cacheGames(games: List<Game>) {
        cachedGames = games
    }

    fun getGames(): List<Game> {
        val games = mutableListOf<Game>()
        val context = CitronApplication.appContext
        preferences = PreferenceManager.getDefaultSharedPreferences(context)

        val gameDirs = mutableListOf<GameDir>()
        val oldGamesDir = preferences.getString(KEY_OLD_GAME_PATH, "") ?: ""
        if (oldGamesDir.isNotEmpty()) {
            val legacyDir = GameDir(oldGamesDir, true)
            if (NativeConfig.getGameDirs().none { it.uriString == legacyDir.uriString }) {
                gameDirs.add(legacyDir)
                NativeConfig.addGameDir(legacyDir)
            }
            preferences.edit().remove(KEY_OLD_GAME_PATH).apply()
        }
        gameDirs.addAll(NativeConfig.getGameDirs())

        // Ensure keys are loaded so that ROM metadata can be decrypted.
        NativeLibrary.reloadKeys()

        // Reset metadata so we don't use stale information
        GameMetadata.resetMetadata()

        // Remove previous filesystem provider information so we can get up to date version info
        NativeLibrary.clearFilesystemProvider()

        gameDirs.forEach { gameDir: GameDir ->
            val gameDirUri = Uri.parse(gameDir.uriString)
            if (FileUtil.isTreeUriValid(gameDirUri)) {
                addGamesRecursive(
                    games,
                    FileUtil.listFiles(gameDirUri),
                    if (gameDir.deepScan) 3 else 1
                )
            } else {
                // A single failed probe (provider restart, transient I/O, ...) must not delete
                // the user's folder registration; keep it and retry on the next reload.
                Log.warning("[GameHelper] Game folder unavailable, keeping it: ${gameDir.uriString}")
            }
        }

        // Do NOT persist `gameDirs` here: it is this scan's snapshot, taken before the scan
        // ran. Adds/removes already save through NativeConfig, and writing the snapshot back
        // would drop a folder added while the scan was in progress.

        // Cache list of games found on disk
        val serializedGames = mutableSetOf<String>()
        games.forEach {
            serializedGames.add(Json.encodeToString(it))
        }
        preferences.edit()
            .remove(KEY_GAMES)
            .putStringSet(KEY_GAMES, serializedGames)
            .apply()

        return games.toList()
    }

    private fun addGamesRecursive(
        games: MutableList<Game>,
        files: Array<MinimalDocumentFile>,
        depth: Int
    ) {
        if (depth <= 0) {
            return
        }

        files.forEach {
            if (it.isDirectory) {
                addGamesRecursive(
                    games,
                    FileUtil.listFiles(it.uri),
                    depth - 1
                )
            } else {
                if (Game.extensions.contains(FileUtil.getExtension(it.uri))) {
                    val game = getGame(it.uri, true)
                    if (game != null) {
                        games.add(game)
                    }
                }
            }
        }
    }

    fun getGame(uri: Uri, addedToLibrary: Boolean): Game? {
        val filePath = uri.toString()
        if (!GameMetadata.getIsValid(filePath)) {
            return null
        }

        // Needed to update installed content information
        NativeLibrary.addFileToFilesystemProvider(filePath)

        var name = GameMetadata.getTitle(filePath)

        // If the game's title field is empty, use the filename.
        if (name.isEmpty()) {
            name = FileUtil.getFilename(uri)
        }
        var programId = GameMetadata.getProgramId(filePath)

        // If the game's ID field is empty, use the filename without extension.
        if (programId.isEmpty()) {
            programId = name.substring(0, name.lastIndexOf("."))
        }

        val newGame = Game(
            name,
            filePath,
            programId,
            GameMetadata.getDeveloper(filePath),
            GameMetadata.getVersion(filePath, false),
            GameMetadata.getIsHomebrew(filePath)
        )

        if (addedToLibrary) {
            val addedTime = preferences.getLong(newGame.keyAddedToLibraryTime, 0L)
            if (addedTime == 0L) {
                preferences.edit()
                    .putLong(newGame.keyAddedToLibraryTime, System.currentTimeMillis())
                    .apply()
            }
        }

        return newGame
    }
}
