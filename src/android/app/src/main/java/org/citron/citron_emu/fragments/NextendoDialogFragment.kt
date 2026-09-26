// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.view.LayoutInflater
import androidx.fragment.app.DialogFragment
import androidx.viewbinding.ViewBinding

// Shared plumbing for the Nextendo dialogs: they all build their layout in onCreateDialog and
// update it from background threads, so they need the same "only while the view exists" guard.
abstract class NextendoDialogFragment<B : ViewBinding> : DialogFragment() {
    private var currentBinding: B? = null

    protected val binding: B
        get() = checkNotNull(currentBinding)

    protected fun inflateBinding(factory: (LayoutInflater) -> B): B =
        factory(layoutInflater).also { currentBinding = it }

    override fun onDestroyView() {
        super.onDestroyView()
        currentBinding = null
    }

    protected fun post(block: () -> Unit) {
        activity?.runOnUiThread {
            if (isAdded && currentBinding != null) {
                block()
            }
        }
    }
}
