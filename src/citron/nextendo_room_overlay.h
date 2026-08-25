// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Nextendo counterpart to util/multiplayer_room_overlay.h: same frameless,
// draggable, always-on-top floating chat window, but for a Nextendo chat room
// instead of an LDN multiplayer session -- and, unlike that overlay, not gated
// on emulation running, since Nextendo rooms are meant to work from the main
// menu too (chatting with a friend who isn't currently playing anything).

#pragma once

#include <QColor>
#include <QJsonObject>
#include <QPoint>
#include <QString>
#include <QWidget>

#include "common/common_types.h"

class GMainWindow;
class QGridLayout;
class QLabel;
class QSizeGrip;
class QToolButton;
class NextendoChatRoom;
class NextendoController;

class NextendoRoomOverlay : public QWidget {
    Q_OBJECT

public:
    explicit NextendoRoomOverlay(QWidget* parent, NextendoController* controller);
    ~NextendoRoomOverlay() override;

    bool IsInRoom() const {
        return !current_room_id.isEmpty();
    }

    void CreateRoom();
    void JoinRoom(const QString& room_id);
    void LeaveRoom();

    // Creates (or reuses, if already hosting) our own room, then invites this
    // player as soon as we're confirmed host -- the path used when a player is
    // invited by clicking them in the friends list rather than typing a code.
    void InviteFriendOnJoin(u64 pid, const QString& name);

    void ShowOverlay(); // brings an already-active room to the front

signals:
    // Fired once a create/join actually lands -- GMainWindow uses this to close
    // whichever NextendoChatWindow launcher dialog triggered it, if any.
    void RoomJoined();
    // "Invite..." clicked in the header -- there's no friend list in this compact
    // overlay, so GMainWindow opens the friends page instead, which already has
    // the real "right-click a friend to invite" flow wired up.
    void InvitePickerRequested();

public slots:
    void UpdateTheme();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    void OnRawMessage(const QJsonObject& obj);

private:
    void UpdatePosition();
    void SendPendingInvite();
    void ResetRoomState();
    void UpdateHeaderText();

    GMainWindow* main_window;
    NextendoController* controller;

    QLabel* header_label;
    QToolButton* invite_button;
    QToolButton* reveal_code_button;
    QToolButton* copy_code_button;
    QToolButton* leave_button;
    bool code_visible = false; // hidden by default -- streamers can be joined by chat-reading randoms otherwise
    NextendoChatRoom* chat_room_widget;
    QGridLayout* main_layout;
    QSizeGrip* size_grip;

    QString current_room_id;
    QString current_room_name;
    bool is_host = false;
    u64 pending_invite_pid = 0;
    QString pending_invite_name;

    QColor background_color;
    QColor border_color;
    int padding = 12;
    int border_width = 1;
    int corner_radius = 10;

    bool is_dragging = false;
    bool has_been_moved = false;
    QPoint drag_start_pos;
};
