// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

// Just the "Create a Room" / "Join with a code" picker -- the room itself lives
// in the persistent NextendoRoomOverlay, not in this dialog. This closes itself
// once GMainWindow confirms the room was actually joined.
class NextendoChatWindow : public QDialog {
    Q_OBJECT

public:
    explicit NextendoChatWindow(QWidget* parent = nullptr);

signals:
    void CreateRoomRequested();
    void JoinRoomRequested(QString room_id);

private:
    QLineEdit* join_code_input;
};
