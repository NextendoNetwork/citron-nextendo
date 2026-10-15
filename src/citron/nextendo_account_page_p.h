// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QStandardItem>
#include <QString>

#include "common/common_types.h"

class NextendoFriendItem : public QStandardItem {
public:
    static constexpr int PidRole = Qt::UserRole + 1;
    static constexpr int NameRole = Qt::UserRole + 2;
    static constexpr int FriendCodeRole = Qt::UserRole + 3;
    static constexpr int PresenceRole = Qt::UserRole + 4;    // s32: 0 offline, 1 online, 2 in-game
    static constexpr int GamePresenceRole = Qt::UserRole + 5; // display name of the running game, may be empty
    static constexpr int AvatarB64Role = Qt::UserRole + 6;
    static constexpr int IsRequestRole = Qt::UserRole + 7;   // true in the incoming-requests list
    static constexpr int PillLabelRole = Qt::UserRole + 8;   // overrides the single-pill's default "Remove" text
    static constexpr int IsMeRole = Qt::UserRole + 9;        // true for the local player's own row; suppresses the pill
    static constexpr int DetailRole = Qt::UserRole + 10;     // replaces the presence line when set
    static constexpr int InviteIdRole = Qt::UserRole + 11;   // game invitation id, invites list only
    static constexpr int PlatformRole = Qt::UserRole + 12;

    static QString PlatformLabel(const QString& platform) {
        if (platform == QStringLiteral("switch")) return QStringLiteral("Nintendo Switch");
        if (platform == QStringLiteral("citron")) return QStringLiteral("Citron");
        if (platform == QStringLiteral("ryujinx")) return QStringLiteral("Ryujinx");
        if (platform == QStringLiteral("emulator")) return QStringLiteral("Emulator");
        return {};
    }

    NextendoFriendItem(u64 pid, const QString& name, const QString& friend_code, s32 presence,
                       const QString& game, const QString& avatar_b64, bool is_request,
                       const QString& pill_label = QString{}, bool is_me = false,
                       const QString& platform = QString{}) {
        setEditable(false);
        setData(QVariant::fromValue<qulonglong>(pid), PidRole);
        setData(name, NameRole);
        setData(friend_code, FriendCodeRole);
        setData(presence, PresenceRole);
        setData(game, GamePresenceRole);
        setData(avatar_b64, AvatarB64Role);
        setData(is_request, IsRequestRole);
        setData(pill_label, PillLabelRole);
        setData(is_me, IsMeRole);
        setData(PlatformLabel(platform), PlatformRole);
    }
};

class NextendoHistoryItem : public QStandardItem {
public:
    static constexpr int TitleIdRole = Qt::UserRole + 1;
    static constexpr int NameRole = Qt::UserRole + 2;
    static constexpr int IconB64Role = Qt::UserRole + 3;
    static constexpr int SecondsRole = Qt::UserRole + 4;
    static constexpr int LastPlayedRole = Qt::UserRole + 5; // RFC3339 string, may be empty

    NextendoHistoryItem(const QString& title_id, const QString& name, const QString& icon_b64,
                        u64 seconds, const QString& last_played) {
        setEditable(false);
        setData(title_id, TitleIdRole);
        setData(name, NameRole);
        setData(icon_b64, IconB64Role);
        setData(QVariant::fromValue<qulonglong>(seconds), SecondsRole);
        setData(last_played, LastPlayedRole);
    }
};
