// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QLocalSocket>
#include <QSharedMemory>
#include <QTimer>

// Discord's local IPC transport. No Discord account credentials are stored.
class DiscordPresence final : public QObject {
public:
    explicit DiscordPresence(QObject* parent);
    void SetActivity(QString details, QString state, QString image = {});
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled; }

private:
    void Connect();
    void Send(quint32 opcode, const QByteArray& json);
    void Publish();
    void Read();
    bool UpdateOwnership();
    QSharedMemory ownership{QStringLiteral("citron-nextendo-discord-owner-v1")};
    QLocalSocket socket;
    QTimer retry;
    QByteArray received;
    QString details;
    QString state;
    QString image;
    bool owns_activity{};
    qint64 started{};
    quint64 nonce{};
    int pipe_index{};
    int connected_pipe_index{};
    bool enabled{true};
    bool ready{};
};
