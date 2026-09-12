// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.utils

import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress

// Speaks the console's nncs NAT-check protocol against the same responders the in-game DNS
// redirect uses (mirrors the desktop's NextendoNetworkProbe). Two distinct answers are needed
// to compare the external mapping at all; one shared mapping means the NAT lets P2P through.
object NextendoConnectionTest {

    enum class NatStatus { OPEN, STRICT, UNKNOWN }

    private const val PROBE_TIMEOUT_MS = 3000
    private const val TEST_ID = 101
    private val PORTS = intArrayOf(10025, 10125)

    fun probeNat(serverIp: String, natIp: String): NatStatus {
        val targets = mutableListOf<Pair<String, Int>>()
        for (host in listOf(serverIp, natIp)) {
            if (host.isEmpty()) {
                continue
            }
            for (port in PORTS) {
                targets.add(host to port)
            }
        }
        if (targets.size < 2) {
            return NatStatus.UNKNOWN
        }

        val payload = ByteArray(16)
        payload[3] = TEST_ID.toByte()
        var answered = 0
        val externalPorts = HashSet<Int>()

        try {
            DatagramSocket().use { socket ->
                socket.soTimeout = PROBE_TIMEOUT_MS
                for ((host, port) in targets) {
                    try {
                        socket.send(
                            DatagramPacket(
                                payload,
                                payload.size,
                                InetAddress.getByName(host),
                                port
                            )
                        )
                    } catch (_: IOException) {
                        // One unreachable responder doesn't invalidate the others.
                    }
                }

                val buffer = ByteArray(64)
                val deadline = System.currentTimeMillis() + PROBE_TIMEOUT_MS
                while (answered < targets.size && System.currentTimeMillis() < deadline) {
                    val packet = DatagramPacket(buffer, buffer.size)
                    try {
                        socket.receive(packet)
                    } catch (_: IOException) {
                        break
                    }
                    if (packet.length < 16 || readU32(packet.data, 0) != TEST_ID) {
                        continue
                    }
                    answered++
                    externalPorts.add(readU32(packet.data, 4))
                }
            }
        } catch (_: IOException) {
            return NatStatus.UNKNOWN
        }

        return when {
            answered < 2 -> NatStatus.UNKNOWN
            externalPorts.size == 1 -> NatStatus.OPEN
            else -> NatStatus.STRICT
        }
    }

    private fun readU32(data: ByteArray, offset: Int): Int =
        ((data[offset].toInt() and 0xFF) shl 24) or
            ((data[offset + 1].toInt() and 0xFF) shl 16) or
            ((data[offset + 2].toInt() and 0xFF) shl 8) or
            (data[offset + 3].toInt() and 0xFF)
}
