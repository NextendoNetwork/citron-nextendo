// SPDX-FileCopyrightText: 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R

// Foreground service so Android doesn't freeze the backgrounded process while
// the OAuth loopback callback is pending in the browser.
class NextendoSignInService : Service() {

    companion object {
        private const val CHANNEL_ID = "nextendo_sign_in"
        private const val NOTIFICATION_ID = 4601

        fun start(context: Context) {
            context.startForegroundService(Intent(context, NextendoSignInService::class.java))
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, NextendoSignInService::class.java))
        }
    }

    override fun onCreate() {
        super.onCreate()
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Nextendo sign-in",
            NotificationManager.IMPORTANCE_LOW
        )
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, buildNotification())
        NativeLibrary.nextendoSignIn()
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun buildNotification(): Notification =
        Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle("Nextendo sign-in")
            .setContentText("Waiting for you to authorize in the browser\u2026")
            .setOngoing(true)
            .build()
}