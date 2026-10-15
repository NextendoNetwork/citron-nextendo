// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QGuiApplication>
#include <QtEndian>
#include "citron/discord_presence.h"
#include "common/logging.h"

namespace {
struct ActivityOwner {
    qint64 pid{};
    qint64 heartbeat{};
};
}

DiscordPresence::DiscordPresence(QObject* parent) : QObject(parent) {
    if (ownership.create(sizeof(ActivityOwner))) {
        ownership.lock();
        *static_cast<ActivityOwner*>(ownership.data()) = {};
        ownership.unlock();
    } else if (!ownership.attach()) {
        LOG_WARNING(Frontend, "Discord activity ownership unavailable");
    }
    connect(&socket, &QLocalSocket::connected, this, [this] {
        connected_pipe_index = (pipe_index + 9) % 10;
        LOG_INFO(Frontend, "Discord IPC connected; sending application handshake");
        Send(0, QJsonDocument(QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("client_id"), QStringLiteral("1559102299564015687")}}).toJson(QJsonDocument::Compact));
    });
    connect(&socket, &QLocalSocket::readyRead, this, &DiscordPresence::Read);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState application_state) {
        if (application_state == Qt::ApplicationActive && enabled) {
            UpdateOwnership();
            Connect();
            Publish();
        }
    });
    connect(&socket, &QLocalSocket::disconnected, this, [this] { ready = false; received.clear(); });
    connect(&socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) { ready = false; received.clear(); });
    retry.setInterval(10000);
    connect(&retry, &QTimer::timeout, this, &DiscordPresence::Connect);
    retry.start();
    SetActivity(tr("In the main menu"), QStringLiteral("Citron | Nextendo"));
    Connect();
}

void DiscordPresence::SetEnabled(bool value) {
    if (enabled == value) return;
    enabled = value;
    if (enabled) Connect();
    else {
        if (ready) {
            Send(1, QJsonDocument(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")}, {QStringLiteral("args"), QJsonObject{{QStringLiteral("pid"), QCoreApplication::applicationPid()}, {QStringLiteral("activity"), QJsonValue::Null}}}, {QStringLiteral("nonce"), QString::number(++nonce)}}).toJson(QJsonDocument::Compact));
        }
        socket.disconnectFromServer();
        ready = false;
    }
}

void DiscordPresence::Connect() {
    if (!enabled || !UpdateOwnership() || socket.state() != QLocalSocket::UnconnectedState) return;
    received.clear();
    const int attempt_index = pipe_index;
    pipe_index = (pipe_index + 1) % 10;
    socket.connectToServer(QStringLiteral("discord-ipc-%1").arg(attempt_index));
}

bool DiscordPresence::UpdateOwnership() {
    if (!ownership.isAttached() || !ownership.lock()) return true;
    auto& owner = *static_cast<ActivityOwner*>(ownership.data());
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto pid = QCoreApplication::applicationPid();
    if (enabled && (owner.pid == 0 || now - owner.heartbeat > 15000 ||
                    QGuiApplication::applicationState() == Qt::ApplicationActive)) {
        owner.pid = pid;
    }
    const bool selected = enabled && owner.pid == pid;
    if (selected) owner.heartbeat = now;
    else if (!enabled && owner.pid == pid) owner = {};
    ownership.unlock();
    if (owns_activity != selected) {
        owns_activity = selected;
        LOG_INFO(Frontend, "Discord activity owner: {}", selected);
        if (!selected) {
            socket.abort();
            ready = false;
            pipe_index = connected_pipe_index;
        }
    }
    return selected;
}

void DiscordPresence::SetActivity(QString new_details, QString new_state, QString new_image) {
    const bool changed = details != new_details || state != new_state || image != new_image;
    details = std::move(new_details);
    state = std::move(new_state);
    image = std::move(new_image);
    if (changed) started = QDateTime::currentSecsSinceEpoch();
    const bool was_owner = owns_activity;
    if (!UpdateOwnership()) return;
    Connect();
    if (changed || !was_owner) Publish();
}

void DiscordPresence::Send(quint32 opcode, const QByteArray& json) {
    if (socket.state() != QLocalSocket::ConnectedState) return;
    QByteArray packet(8, '\0');
    qToLittleEndian(opcode, packet.data());
    qToLittleEndian(static_cast<quint32>(json.size()), packet.data() + 4);
    packet += json;
    socket.write(packet);
}

void DiscordPresence::Publish() {
    if (!ready || !enabled || !UpdateOwnership()) return;
    QJsonObject activity{
        {QStringLiteral("details"), details.left(128)}, {QStringLiteral("state"), state.left(128)},
        {QStringLiteral("timestamps"), QJsonObject{{QStringLiteral("start"), started}}},
        {QStringLiteral("buttons"), QJsonArray{QJsonObject{{QStringLiteral("label"), QStringLiteral("Nextendo Page")}, {QStringLiteral("url"), QStringLiteral("https://nextendo.network/compte")}}}}};
    if (!image.isEmpty()) {
        activity.insert(QStringLiteral("assets"), QJsonObject{
            {QStringLiteral("large_image"), image},
            {QStringLiteral("large_text"), details.left(128)}});
    }
    LOG_INFO(Frontend, "Discord publishing activity: details={} image={}",
             details.toStdString(), image.toStdString());
    Send(1, QJsonDocument(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
         {QStringLiteral("args"), QJsonObject{{QStringLiteral("pid"), QCoreApplication::applicationPid()}, {QStringLiteral("activity"), activity}}},
         {QStringLiteral("nonce"), QString::number(++nonce)}}).toJson(QJsonDocument::Compact));
}

void DiscordPresence::Read() {
    received += socket.readAll();
    while (received.size() >= 8) {
        const auto opcode = qFromLittleEndian<quint32>(received.constData());
        const auto length = qFromLittleEndian<quint32>(received.constData() + 4);
        if (length > 65536) { socket.abort(); received.clear(); return; }
        if (received.size() < static_cast<qsizetype>(length) + 8) return;
        const auto payload = received.mid(8, length);
        received.remove(0, length + 8);
        if (opcode == 3) Send(4, payload);
        else if (opcode == 2) {
            const auto close = QJsonDocument::fromJson(payload).object();
            LOG_WARNING(Frontend, "Discord IPC closed: code={} message={}",
                        close.value(QStringLiteral("code")).toInt(),
                        close.value(QStringLiteral("message")).toString().toStdString());
            socket.abort(); ready = false; return;
        } else if (opcode == 1) {
            const auto response = QJsonDocument::fromJson(payload).object();
            const auto event = response.value(QStringLiteral("evt")).toString();
            if (event == QStringLiteral("READY")) {
                LOG_INFO(Frontend, "Discord IPC ready");
                ready = true;
                Publish();
            } else if (event == QStringLiteral("ERROR")) {
                const auto error = response.value(QStringLiteral("data")).toObject();
                LOG_WARNING(Frontend, "Discord rejected activity: code={} message={}",
                            error.value(QStringLiteral("code")).toInt(),
                            error.value(QStringLiteral("message")).toString().toStdString());
            } else if (response.value(QStringLiteral("cmd")) == QStringLiteral("SET_ACTIVITY")) {
                LOG_INFO(Frontend, "Discord accepted activity update");
            }
        }
    }
}
