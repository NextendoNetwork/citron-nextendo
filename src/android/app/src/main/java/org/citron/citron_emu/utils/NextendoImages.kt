// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.utils

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Base64

// The account server hands avatars and game icons out as base64 JPEGs; every screen that shows
// one needs the same tolerant decode.
object NextendoImages {
    fun decode(base64: String): Bitmap? = if (base64.isEmpty()) {
        null
    } else {
        try {
            val bytes = Base64.decode(base64, Base64.DEFAULT)
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        } catch (_: IllegalArgumentException) {
            null
        }
    }
}
