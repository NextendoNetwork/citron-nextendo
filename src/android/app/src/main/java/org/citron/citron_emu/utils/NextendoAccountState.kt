// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.utils

import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.json.JSONObject

data class NextendoProfile(
    val name: String,
    val consoleNickname: String,
    val imageBase64: String,
    val avatarId: String,
    val colorHex: String,
    val friendCode: String,
    val pid: Long
)

// The linked account's profile, fetched once and cached so screens don't each query it, plus a
// change signal anything account-related can collect. Refreshed at startup, after sign-in and
// after profile edits.
object NextendoAccountState {
    @Volatile
    var profile: NextendoProfile? = null
        private set

    @Volatile
    var avatar: Bitmap? = null
        private set

    private val _generation = MutableStateFlow(0)
    val generation: StateFlow<Int> = _generation.asStateFlow()

    fun notifyChanged() {
        _generation.value++
    }

    fun clear() {
        profile = null
        avatar = null
        notifyChanged()
    }

    fun refresh() {
        if (NativeLibrary.getNextendoAccountStatus().isEmpty()) {
            return
        }
        Thread {
            val fetched = NativeLibrary.nextendoGetProfileJson()

            // The API clears the stored account when the server rejects its token, so an empty
            // link state here means the session expired rather than a transient failure.
            if (NativeLibrary.getNextendoAccountStatus().isEmpty()) {
                profile = null
                avatar = null
                Handler(Looper.getMainLooper()).post {
                    Toast.makeText(
                        CitronApplication.appContext,
                        R.string.nextendo_session_expired,
                        Toast.LENGTH_LONG
                    ).show()
                }
                notifyChanged()
                return@Thread
            }

            val parsed = parse(fetched) ?: return@Thread
            profile = parsed
            avatar = NextendoImages.decode(parsed.imageBase64)
            notifyChanged()
        }.start()
    }

    private fun parse(json: String): NextendoProfile? = try {
        val entry = JSONObject(json)
        if (!entry.optBoolean("ok")) {
            null
        } else {
            NextendoProfile(
                name = entry.optString("name"),
                consoleNickname = entry.optString("console_nickname"),
                imageBase64 = entry.optString("image"),
                avatarId = entry.optString("avatar_id"),
                colorHex = entry.optString("color"),
                friendCode = entry.optString("friend_code"),
                pid = entry.optLong("pid")
            )
        }
    } catch (_: Exception) {
        null
    }
}
