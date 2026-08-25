// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QStandardItem>
#include <QStyledItemDelegate>

#include "common/common_types.h"

// Member-list row model for NextendoChatRoom. Mirrors PlayerListItem from the LDN
// chat_room.cpp, but keyed by PID (with a real fetched avatar) instead of nickname
// (with a letter-avatar placeholder).
class NextendoChatMemberItem : public QStandardItem {
public:
    static constexpr int PidRole = Qt::UserRole + 1;
    static constexpr int NameRole = Qt::UserRole + 2;
    static constexpr int AvatarB64Role = Qt::UserRole + 3;
    static constexpr int IsHostRole = Qt::UserRole + 4;
    static constexpr int IsMutedRole = Qt::UserRole + 5;
    static constexpr int IsMeRole = Qt::UserRole + 6;
    static constexpr int HighlightOpacityRole = Qt::UserRole + 7; // 0..1, fades after speaking

    NextendoChatMemberItem(u64 pid, const QString& name, bool is_host, bool is_muted, bool is_me) {
        setEditable(false);
        setData(QVariant::fromValue<qulonglong>(pid), PidRole);
        setData(name, NameRole);
        setData(is_host, IsHostRole);
        setData(is_muted, IsMutedRole);
        setData(is_me, IsMeRole);
        setData(0.0f, HighlightOpacityRole);
    }
};

// Rework of ChatRoomMemberDelegate (multiplayer/chat_room_member_delegate.h) for
// Nextendo chat rooms: draws a real fetched profile picture (falling back to the
// same letter-avatar look while it loads), a gold ring for the host, and a MUTED
// badge -- everything else (card background, hover/selection, layout) matches the
// LDN member list so the two chat surfaces feel like the same feature.
class NextendoChatRoomMemberDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit NextendoChatRoomMemberDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    static constexpr int kAvatarSize = 40;
    static constexpr int kRowHeight = 52;
};
