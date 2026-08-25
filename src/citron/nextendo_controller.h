// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Common {
struct UUID;
}

class NextendoChatClient;

// Sign-in/out, friend-cache refresh, and the online-toast poll. One instance, owned by GMainWindow.
class NextendoController : public QObject {
    Q_OBJECT

public:
    explicit NextendoController(Core::System& system, QWidget* main_window,
                                QObject* parent = nullptr);
    ~NextendoController() override;

    bool IsLinked() const;

    // Title id -> display name via the locally installed game's NACP. Falls back to hint_name
    // (a name the PLAYING client already resolved, e.g. from a friend's presence) when this
    // game isn't in the local library; falls back further to a generic "a game" if both fail.
    QString ResolveGameName(const std::string& app_id_hex, const std::string& hint_name = {}) const;

    // Title id -> base64 icon via the locally installed game's NACP; empty if not installed.
    QString ResolveGameIcon(const std::string& app_id_hex) const;

    // 16 hex digits of the locally running title, matching Friend::app_id; empty if not running.
    std::string GetLocalAppId() const;

    void SignIn();
    void SignOut();
    void RefreshFriendCache();
    void NotifyFriendRequestSent(const QString& friend_code);

    void ManualSaveDownload(u64 title_id);
    void QuickStart(u64 title_id);

    // Chat: one persistent connection for the whole session (not tied to any dialog's
    // lifetime), so invite/join notifications work the same way FriendRequestReceived
    // does -- in the background, regardless of whether the chat window is open. Idempotent.
    void EnsureChatConnected();
    NextendoChatClient* GetChatClient() const {
        return chat_client;
    }

signals:
    void AccountLinked();
    void AccountUnlinked();
    void FriendCameOnline(u64 pid, QString name, QString game_name, QString avatar_base64);
    void FriendWentOffline(u64 pid, QString name, QString avatar_base64);
    void FriendRequestReceived(u64 pid, QString name, QString avatar_base64);
    void FriendRequestSent(QString friend_code);
    void StatusChanged(QString message);
    void QuickStartRequested(u64 title_id);
    // Fired once the OAuth URL is known, whether or not the OS actually opened a visible
    // browser window for it (xdg-open/openUrl can both report success with nothing appearing).
    void SignInUrlReady(QString url);
    void SignInFinished();

    // Chat notifications -- mirror the FriendRequest* pair above so main.cpp can route them
    // to the same shared NextendoToast the same way.
    void ChatInviteReceived(QString room_id, QString room_name, u64 from_pid, QString from_name);
    void ChatInviteSent(u64 target_pid);
    void ChatMemberJoined(QString room_id, u64 pid, QString name);
    void ChatBanned(QString reason);
    // Raw pass-through for whichever chat dialog is currently open (room_joined,
    // chat_message, member_left, kicked, muted, room_closed, error, ...) -- the dialog
    // itself decides what it cares about instead of this class knowing every message shape.
    void ChatRawMessage(QJsonObject obj);

private:
    void ApplyProfileName(const std::string& name);
    // Forces the active Switch profile's picture to match the linked Nextendo account's
    // avatar, mirroring ApplyProfileName's username sync. Fetches async since it needs a
    // network round-trip; WriteProfileAvatar does the actual disk write once it lands.
    void SyncProfileAvatar();
    void WriteProfileAvatar(const Common::UUID& uuid, const std::string& avatar_b64);
    void PollFriends();

    Core::System& system;
    QWidget* main_window;
    QTimer friend_poll_timer;
    std::map<u64, s32> last_known_status;
    std::map<u64, int> offline_streak; // consecutive polls seen offline, not yet confirmed
    std::set<u64> last_known_requests;
    bool first_poll = true; // suppresses a toast burst for every friend already online at boot

    NextendoChatClient* chat_client = nullptr;
    QString pending_chat_room_id; // set by whichever create/join is in flight, used to tag ChatMemberJoined
};
