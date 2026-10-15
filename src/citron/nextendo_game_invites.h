// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include <QObject>
#include <QString>
#include <QTimer>
#include "web_service/nextendo_api.h"

namespace Core {
class System;
}

// Incoming game invitations, kept until accepted, declined or expired (5 minutes server-side).
class NextendoGameInvites final : public QObject {
    Q_OBJECT
public:
    using Invite = WebService::NextendoApi::GameInvitation;

    explicit NextendoGameInvites(Core::System& system, QObject* parent = nullptr);

    // Unexpired invites, newest first.
    std::vector<Invite> Pending() const;
    QString Avatar(u64 pid) const;
    void Refresh();
    // Empty on success, otherwise why the invite can't be joined right now.
    QString Accept(const QString& id);
    void Decline(const QString& id);

signals:
    void Changed();
    // A new invitation, including while HOME is displayed.
    void Received(QString id, QString sender_name, u64 sender_pid);

private:
    bool PruneExpired();
    void Remove(const QString& id);

    Core::System& system;
    QTimer poll_timer;
    std::vector<Invite> invites;
    std::set<std::string> seen;
    std::map<u64, std::string> avatars;
    u64 generation{};
    bool fetching{};
    QString launching_invitation;
    u64 launching_generation{};
};
