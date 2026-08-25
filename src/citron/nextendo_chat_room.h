// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rework of multiplayer/chat_room.h for Nextendo chat rooms: same widget (member
// list, chat bubbles, emoji picker, moderation menu, highlight animation), but
// driven by NextendoChatClient's WebSocket JSON instead of Network::RoomMember.

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <QColor>
#include <QJsonArray>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

#include "common/common_types.h"

namespace Ui {
class NextendoChatRoom;
}

class QModelIndex;
class QPushButton;
class QStandardItem;
class QStandardItemModel;
class QVBoxLayout;
class NextendoChatClient;
class NextendoChatRoomMemberDelegate;

class NextendoChatRoom : public QWidget {
    Q_OBJECT

public:
    explicit NextendoChatRoom(QWidget* parent);
    ~NextendoChatRoom() override;

    void SetChatClient(NextendoChatClient* client);
    void SetLocalIdentity(u64 pid, const QString& name);
    void SetModPerms(bool is_mod); // true while we're the room host
    void SetShowOptions(bool show); // hides the "Options" gear button when space is tight (overlay)
    void SetMembers(const QJsonArray& members);
    void AddHistoryMessage(u64 pid, const QString& name, const QString& text);
    void Clear();
    void AppendStatusMessage(const QString& msg);
    void UpdateTheme();

public slots:
    void OnChatMessage(u64 pid, const QString& name, const QString& text);
    void OnMemberJoined(u64 pid, const QString& name);
    void OnMemberLeft(u64 pid);
    void OnMemberUpdated(u64 pid, bool muted);
    void OnSendChat();

private slots:
    void OnChatTextChanged();
    void PopupContextMenu(const QPoint& menu_location);
    void OnChatContextMenu(const QPoint& menu_location);

private:
    void AppendChatMessage(const QString& html_msg, u64 sender_pid, const QColor& color);
    bool ValidateMessage(const std::string& msg);
    std::string SanitizeMessage(const std::string& message);
    QColor GetPlayerColor(u64 pid, const QString& name) const;
    void HighlightPlayer(u64 pid);
    void FetchAvatar(u64 pid);
    void ReportPlayer(u64 pid, const QString& name);
    QStandardItem* FindMemberItem(u64 pid) const;

    struct HighlightState {
        QPointer<QVariantAnimation> animation;
        QPointer<QTimer> linger_timer;
        float opacity = 0.0f;
    };

    static constexpr auto THROTTLE_INTERVAL = std::chrono::seconds(5);
    static constexpr size_t MAX_MESSAGES_PER_INTERVAL = 3;

    std::unique_ptr<Ui::NextendoChatRoom> ui;
    NextendoChatClient* chat_client = nullptr;
    u64 my_pid = 0;
    QString my_name;

    QPushButton* send_message;
    QStandardItemModel* member_model;
    NextendoChatRoomMemberDelegate* member_delegate;
    QWidget* chat_container;
    QVBoxLayout* chat_layout;

    bool has_mod_perms = false;
    std::set<u64> block_list;
    bool chat_muted = false;
    size_t max_chat_lines = 1000;
    bool show_timestamps = true;
    bool member_scrollbar_hidden = false;

    std::map<u64, HighlightState> highlight_states;
    std::map<u64, std::string> color_overrides;
    std::vector<std::chrono::steady_clock::time_point> sent_message_timestamps;
    std::unordered_map<u64, QString> avatar_cache; // pid -> base64
};
