// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QTcpSocket;

// Minimal RFC 6455 WebSocket client for talking to the Nextendo chat-room
// service (a plain JSON-over-text-frame protocol, not NEX/PRUDP). No Qt
// WebSockets module is linked into this project (only Core/Widgets/Concurrent/
// Network/Svg) and adding one risks a full Qt rebuild, so this hand-rolls the
// handshake and frame (de)masking on top of the already-linked QTcpSocket --
// the same approach this codebase already takes for PRUDP-Lite-over-WebSocket
// server-side. Text frames only; assumes unfragmented messages, which is all
// the chat server ever sends.
class NextendoChatClient : public QObject {
    Q_OBJECT

public:
    explicit NextendoChatClient(QObject* parent = nullptr);
    ~NextendoChatClient() override;

    void Connect(const QString& host, quint16 port);
    void Disconnect();
    bool IsConnected() const;

    void SendJson(const QJsonObject& obj);

signals:
    void Connected();
    void Disconnected();
    void MessageReceived(const QJsonObject& obj);
    void ConnectionError(const QString& message);

private:
    void OnSocketConnected();
    void OnReadyRead();
    void OnSocketError();
    void ProcessHandshakeResponse();
    void ProcessFrames();
    void SendFrame(uint8_t opcode, const QByteArray& payload);

    QTcpSocket* socket;
    QByteArray recv_buffer;
    QByteArray expected_accept;
    bool handshake_done = false;
};
