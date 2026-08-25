// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class QObject;

// Polls Nazi Zombies Portable's own nextendo-nzp server for a total online player count.
namespace Nextendo::NzpOnlineCount {

void Start(QObject* parent);

int Get();

} // namespace Nextendo::NzpOnlineCount
