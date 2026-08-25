// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_chat_client.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTcpSocket>

namespace {
constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

QByteArray MakeWebSocketKey() {
    QByteArray key(16, Qt::Uninitialized);
    for (int i = 0; i < 16; ++i) {
        key[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    }
    return key.toBase64();
}
} // namespace

NextendoChatClient::NextendoChatClient(QObject* parent) : QObject(parent) {
    socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::connected, this, &NextendoChatClient::OnSocketConnected);
    connect(socket, &QTcpSocket::readyRead, this, &NextendoChatClient::OnReadyRead);
    connect(socket, &QTcpSocket::errorOccurred, this, &NextendoChatClient::OnSocketError);
    connect(socket, &QTcpSocket::disconnected, this, [this] {
        handshake_done = false;
        emit Disconnected();
    });
}

NextendoChatClient::~NextendoChatClient() = default;

void NextendoChatClient::Connect(const QString& host, quint16 port) {
    if (socket->state() != QAbstractSocket::UnconnectedState) {
        socket->abort();
    }
    handshake_done = false;
    recv_buffer.clear();
    socket->connectToHost(host, port);
}

void NextendoChatClient::Disconnect() {
    socket->disconnectFromHost();
}

bool NextendoChatClient::IsConnected() const {
    return handshake_done && socket->state() == QAbstractSocket::ConnectedState;
}

void NextendoChatClient::OnSocketConnected() {
    const QByteArray key = MakeWebSocketKey();
    const QByteArray concat_key = key + QByteArray(kWebSocketGuid);
    expected_accept = QCryptographicHash::hash(concat_key, QCryptographicHash::Sha1).toBase64();

    const QString request = QStringLiteral("GET / HTTP/1.1\r\n"
                                            "Host: %1:%2\r\n"
                                            "Upgrade: websocket\r\n"
                                            "Connection: Upgrade\r\n"
                                            "Sec-WebSocket-Key: %3\r\n"
                                            "Sec-WebSocket-Version: 13\r\n\r\n")
                                 .arg(socket->peerName().isEmpty() ? socket->peerAddress().toString()
                                                                    : socket->peerName())
                                 .arg(socket->peerPort())
                                 .arg(QString::fromLatin1(key));
    socket->write(request.toUtf8());
}

void NextendoChatClient::OnReadyRead() {
    recv_buffer.append(socket->readAll());
    if (!handshake_done) {
        ProcessHandshakeResponse();
        if (!handshake_done) {
            return; // headers not fully received yet
        }
    }
    ProcessFrames();
}

void NextendoChatClient::ProcessHandshakeResponse() {
    const int header_end = recv_buffer.indexOf("\r\n\r\n");
    if (header_end < 0) {
        return; // wait for more data
    }
    const QByteArray headers = recv_buffer.left(header_end);
    recv_buffer.remove(0, header_end + 4);

    if (!headers.startsWith("HTTP/1.1 101") && !headers.startsWith("HTTP/1.0 101")) {
        emit ConnectionError(QStringLiteral("Chat server rejected the WebSocket handshake."));
        socket->abort();
        return;
    }
    // Not fatal if this doesn't match exactly (some proxies rewrite headers'
    // casing) -- the 101 status is the real signal we're on a WS connection.
    // Still checked so a genuinely wrong server is caught instead of silently
    // trying to parse whatever it sends as WS frames.
    bool accept_ok = false;
    for (const QByteArray& line : headers.split('\n')) {
        if (line.toLower().startsWith("sec-websocket-accept:")) {
            const QByteArray value = line.mid(line.indexOf(':') + 1).trimmed();
            accept_ok = (value == expected_accept);
            break;
        }
    }
    if (!accept_ok) {
        emit ConnectionError(QStringLiteral("Chat server handshake key mismatch."));
        socket->abort();
        return;
    }

    handshake_done = true;
    emit Connected();
}

void NextendoChatClient::ProcessFrames() {
    while (true) {
        if (recv_buffer.size() < 2) {
            return;
        }
        const uint8_t byte0 = static_cast<uint8_t>(recv_buffer[0]);
        const uint8_t byte1 = static_cast<uint8_t>(recv_buffer[1]);
        const uint8_t opcode = byte0 & 0x0F;
        const bool masked = (byte1 & 0x80) != 0; // server frames should never be masked
        uint64_t payload_len = byte1 & 0x7F;

        size_t header_len = 2;
        if (payload_len == 126) {
            if (recv_buffer.size() < 4) {
                return;
            }
            payload_len = (static_cast<uint8_t>(recv_buffer[2]) << 8) |
                          static_cast<uint8_t>(recv_buffer[3]);
            header_len = 4;
        } else if (payload_len == 127) {
            if (recv_buffer.size() < 10) {
                return;
            }
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | static_cast<uint8_t>(recv_buffer[2 + i]);
            }
            header_len = 10;
        }
        if (masked) {
            header_len += 4;
        }
        if (static_cast<uint64_t>(recv_buffer.size()) < header_len + payload_len) {
            return; // wait for the rest of this frame
        }

        QByteArray payload = recv_buffer.mid(static_cast<int>(header_len), static_cast<int>(payload_len));
        if (masked) {
            const char* mask_key = recv_buffer.constData() + (header_len - 4);
            for (int i = 0; i < payload.size(); ++i) {
                payload[i] = payload[i] ^ mask_key[i % 4];
            }
        }
        recv_buffer.remove(0, static_cast<int>(header_len + payload_len));

        switch (opcode) {
        case 0x1: { // text
            const QJsonDocument doc = QJsonDocument::fromJson(payload);
            if (doc.isObject()) {
                emit MessageReceived(doc.object());
            }
            break;
        }
        case 0x8: // close
            socket->disconnectFromHost();
            return;
        case 0x9: // ping -> pong
            SendFrame(0xA, payload);
            break;
        default:
            break; // binary/pong/continuation: unused by this protocol
        }
    }
}

void NextendoChatClient::SendFrame(uint8_t opcode, const QByteArray& payload) {
    QByteArray frame;
    frame.append(static_cast<char>(0x80 | (opcode & 0x0F))); // FIN=1

    const int len = payload.size();
    uint8_t mask_key[4];
    for (auto& b : mask_key) {
        b = static_cast<uint8_t>(QRandomGenerator::global()->bounded(256));
    }

    if (len < 126) {
        frame.append(static_cast<char>(0x80 | len)); // MASK=1 (client frames must be masked)
    } else if (len < 65536) {
        frame.append(static_cast<char>(0x80 | 126));
        frame.append(static_cast<char>((len >> 8) & 0xFF));
        frame.append(static_cast<char>(len & 0xFF));
    } else {
        frame.append(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) {
            frame.append(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }
    frame.append(reinterpret_cast<const char*>(mask_key), 4);

    QByteArray masked_payload = payload;
    for (int i = 0; i < masked_payload.size(); ++i) {
        masked_payload[i] = masked_payload[i] ^ mask_key[i % 4];
    }
    frame.append(masked_payload);

    socket->write(frame);
}

void NextendoChatClient::SendJson(const QJsonObject& obj) {
    if (!IsConnected()) {
        return;
    }
    SendFrame(0x1, QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void NextendoChatClient::OnSocketError() {
    emit ConnectionError(socket->errorString());
}
