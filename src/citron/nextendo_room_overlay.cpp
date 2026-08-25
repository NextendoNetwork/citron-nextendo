// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSizeGrip>
#include <QTimer>
#include <QToolButton>
#include <QWindow>

#include "citron/main.h"
#include "citron/nextendo_chat_client.h"
#include "citron/nextendo_chat_room.h"
#include "citron/nextendo_controller.h"
#include "citron/nextendo_room_overlay.h"
#include "citron/uisettings.h"
#include "common/nextendo_account.h"

NextendoRoomOverlay::NextendoRoomOverlay(QWidget* parent, NextendoController* controller_)
    : QWidget(parent), controller(controller_) {
    main_window = qobject_cast<GMainWindow*>(parent->window());

    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);

    main_layout = new QGridLayout(this);
    main_layout->setContentsMargins(padding, padding, padding, padding);
    main_layout->setSpacing(8);

    auto* header_row = new QWidget(this);
    auto* header_layout = new QHBoxLayout(header_row);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(4);

    header_label = new QLabel(tr("No active room."), header_row);
    header_label->setFont(QFont(QString::fromUtf8("Segoe UI"), 11, QFont::Bold));
    header_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* shadow = new QGraphicsDropShadowEffect(header_row);
    shadow->setBlurRadius(6);
    shadow->setColor(Qt::black);
    shadow->setOffset(0, 0);
    header_label->setGraphicsEffect(shadow);
    header_layout->addWidget(header_label, 1);

    invite_button = new QToolButton(header_row);
    invite_button->setText(QStringLiteral("+"));
    invite_button->setToolTip(tr("Invite a Friend"));
    invite_button->setAutoRaise(true);
    invite_button->setVisible(false);
    connect(invite_button, &QToolButton::clicked, this,
            [this] { emit InvitePickerRequested(); });
    header_layout->addWidget(invite_button);

    reveal_code_button = new QToolButton(header_row);
    reveal_code_button->setText(QStringLiteral("👁"));
    reveal_code_button->setToolTip(tr("Show/Hide Room Code"));
    reveal_code_button->setAutoRaise(true);
    reveal_code_button->setVisible(false);
    connect(reveal_code_button, &QToolButton::clicked, this, [this] {
        code_visible = !code_visible;
        UpdateHeaderText();
    });
    header_layout->addWidget(reveal_code_button);

    copy_code_button = new QToolButton(header_row);
    copy_code_button->setText(QStringLiteral("⧉"));
    copy_code_button->setToolTip(tr("Copy Room Code"));
    copy_code_button->setAutoRaise(true);
    copy_code_button->setVisible(false);
    connect(copy_code_button, &QToolButton::clicked, this,
            [this] { QApplication::clipboard()->setText(current_room_id); });
    header_layout->addWidget(copy_code_button);

    // "─" only hides the overlay -- you're still in the room in the background,
    // same as minimizing any other window. Actually leaving needs a distinct,
    // explicit action so booting a game (which used to route through this same
    // button) can't silently kick you out of the room.
    auto* hide_button = new QToolButton(header_row);
    hide_button->setText(QStringLiteral("─"));
    hide_button->setToolTip(tr("Hide (stay in the room)"));
    hide_button->setAutoRaise(true);
    connect(hide_button, &QToolButton::clicked, this, [this] { hide(); });
    header_layout->addWidget(hide_button);

    leave_button = new QToolButton(header_row);
    leave_button->setText(QStringLiteral("✕"));
    leave_button->setToolTip(tr("Leave Room"));
    leave_button->setAutoRaise(true);
    leave_button->setVisible(false);
    connect(leave_button, &QToolButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Leave Room"),
                                  tr("Leave \"%1\"? You'll need the code to rejoin.")
                                      .arg(current_room_name)) == QMessageBox::Yes) {
            LeaveRoom();
        }
    });
    header_layout->addWidget(leave_button);

    chat_room_widget = new NextendoChatRoom(this);
    chat_room_widget->SetShowOptions(false);
    chat_room_widget->SetChatClient(controller->GetChatClient());
    chat_room_widget->SetLocalIdentity(Common::NextendoAccount::GetPid(),
                                       QString::fromStdString(Common::NextendoAccount::GetUsername()));
    size_grip = new QSizeGrip(this);
    size_grip->setFixedSize(16, 16);

    main_layout->addWidget(header_row, 0, 0, 1, 2);
    main_layout->addWidget(chat_room_widget, 1, 0, 1, 2);
    main_layout->addWidget(size_grip, 1, 1, 1, 1, Qt::AlignBottom | Qt::AlignRight);
    main_layout->setRowStretch(1, 1);
    main_layout->setColumnStretch(0, 1);
    setLayout(main_layout);

    connect(controller, &NextendoController::ChatRawMessage, this,
            &NextendoRoomOverlay::OnRawMessage);

    UpdateTheme();

    const bool is_gamescope = UISettings::IsGamescope();
    if (is_gamescope) {
        setMinimumSize(450, 350);
        resize(700, 550);
        this->padding = 15;
    } else {
        setMinimumSize(400, 300);
        resize(440, 340);
    }

    hide();
    UpdatePosition();
}

NextendoRoomOverlay::~NextendoRoomOverlay() = default;

void NextendoRoomOverlay::ResetRoomState() {
    current_room_id.clear();
    current_room_name.clear();
    is_host = false;
    code_visible = false;
    header_label->setText(tr("No active room."));
    invite_button->setVisible(false);
    reveal_code_button->setVisible(false);
    copy_code_button->setVisible(false);
    leave_button->setVisible(false);
    chat_room_widget->Clear();
}

void NextendoRoomOverlay::UpdateHeaderText() {
    if (current_room_id.isEmpty()) {
        return;
    }
    const QString code_part =
        code_visible ? current_room_id : QString(current_room_id.length(), QChar(0x2022)); // •
    header_label->setText(tr("%1  ·  %2").arg(current_room_name, code_part));
    reveal_code_button->setText(code_visible ? QStringLiteral("🙈") : QStringLiteral("👁"));
}

void NextendoRoomOverlay::CreateRoom() {
    if (!current_room_id.isEmpty()) {
        return; // already hosting/in a room
    }
    controller->EnsureChatConnected();
    auto* client = controller->GetChatClient();
    if (!client) {
        return;
    }
    chat_room_widget->SetChatClient(client);
    if (client->IsConnected()) {
        client->SendJson(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("create_room")},
            {QStringLiteral("room_name"),
             tr("%1's Room").arg(QString::fromStdString(Common::NextendoAccount::GetUsername()))},
        });
    } else {
        connect(client, &NextendoChatClient::Connected, this, [this] { CreateRoom(); },
                Qt::SingleShotConnection);
    }
}

void NextendoRoomOverlay::JoinRoom(const QString& room_id) {
    if (room_id.isEmpty()) {
        return;
    }
    controller->EnsureChatConnected();
    auto* client = controller->GetChatClient();
    if (!client) {
        return;
    }
    chat_room_widget->SetChatClient(client);
    if (client->IsConnected()) {
        client->SendJson(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("join_room")},
            {QStringLiteral("room_id"), room_id},
        });
    } else {
        connect(client, &NextendoChatClient::Connected, this, [this, room_id] { JoinRoom(room_id); },
                Qt::SingleShotConnection);
    }
}

void NextendoRoomOverlay::LeaveRoom() {
    if (current_room_id.isEmpty()) {
        hide();
        return;
    }
    if (auto* client = controller->GetChatClient()) {
        client->SendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("leave_room")}});
    }
    ResetRoomState();
    hide();
}

void NextendoRoomOverlay::InviteFriendOnJoin(u64 pid, const QString& name) {
    pending_invite_pid = pid;
    pending_invite_name = name;
    if (!current_room_id.isEmpty() && is_host) {
        SendPendingInvite();
        return;
    }
    CreateRoom();
}

void NextendoRoomOverlay::SendPendingInvite() {
    if (pending_invite_pid == 0) {
        return;
    }
    if (auto* client = controller->GetChatClient()) {
        client->SendJson(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("invite_player")},
            {QStringLiteral("target_pid"), static_cast<double>(pending_invite_pid)},
        });
    }
    pending_invite_pid = 0;
    pending_invite_name.clear();
}

void NextendoRoomOverlay::ShowOverlay() {
    show();
    raise();
    if (!has_been_moved) {
        UpdatePosition();
    }
}

void NextendoRoomOverlay::OnRawMessage(const QJsonObject& obj) {
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("room_joined")) {
        current_room_id = obj.value(QStringLiteral("room_id")).toString();
        current_room_name = obj.value(QStringLiteral("room_name")).toString();
        is_host = obj.value(QStringLiteral("is_host")).toBool();
        code_visible = false; // hidden by default -- avoids leaking it on a stream/screenshot
        UpdateHeaderText();
        invite_button->setVisible(is_host); // only the host may invite (server-enforced too)
        reveal_code_button->setVisible(true);
        copy_code_button->setVisible(true);
        leave_button->setVisible(true);

        chat_room_widget->SetChatClient(controller->GetChatClient());
        chat_room_widget->Clear();
        chat_room_widget->SetModPerms(is_host);
        chat_room_widget->SetMembers(obj.value(QStringLiteral("members")).toArray());
        for (const auto& v : obj.value(QStringLiteral("history")).toArray()) {
            const auto h = v.toObject();
            chat_room_widget->AddHistoryMessage(
                static_cast<u64>(h.value(QStringLiteral("pid")).toDouble()),
                h.value(QStringLiteral("name")).toString(),
                h.value(QStringLiteral("text")).toString());
        }

        ShowOverlay();
        emit RoomJoined();

        if (is_host) {
            SendPendingInvite();
        }

    } else if (type == QStringLiteral("chat_message")) {
        if (obj.value(QStringLiteral("room_id")).toString() != current_room_id) {
            return;
        }
        chat_room_widget->OnChatMessage(static_cast<u64>(obj.value(QStringLiteral("pid")).toDouble()),
                                        obj.value(QStringLiteral("name")).toString(),
                                        obj.value(QStringLiteral("text")).toString());

    } else if (type == QStringLiteral("member_joined")) {
        if (current_room_id.isEmpty()) {
            return;
        }
        chat_room_widget->OnMemberJoined(
            static_cast<u64>(obj.value(QStringLiteral("pid")).toDouble()),
            obj.value(QStringLiteral("name")).toString());

    } else if (type == QStringLiteral("member_left")) {
        if (current_room_id.isEmpty()) {
            return;
        }
        chat_room_widget->OnMemberLeft(static_cast<u64>(obj.value(QStringLiteral("pid")).toDouble()));

    } else if (type == QStringLiteral("member_updated")) {
        if (current_room_id.isEmpty()) {
            return;
        }
        chat_room_widget->OnMemberUpdated(static_cast<u64>(obj.value(QStringLiteral("pid")).toDouble()),
                                          obj.value(QStringLiteral("muted")).toBool());

    } else if (type == QStringLiteral("kicked")) {
        if (current_room_id.isEmpty()) {
            return;
        }
        chat_room_widget->AppendStatusMessage(tr("You were removed from this room."));
        QTimer::singleShot(2000, this, [this] { ResetRoomState(); hide(); });

    } else if (type == QStringLiteral("room_closed")) {
        if (current_room_id.isEmpty()) {
            return;
        }
        chat_room_widget->AppendStatusMessage(tr("The host closed this room."));
        QTimer::singleShot(2000, this, [this] { ResetRoomState(); hide(); });

    } else if (type == QStringLiteral("error")) {
        const QString message = obj.value(QStringLiteral("message")).toString();
        if (message == QStringLiteral("target_offline")) {
            pending_invite_pid = 0;
            pending_invite_name.clear();
        } else if (message == QStringLiteral("invite_cooldown")) {
            chat_room_widget->AppendStatusMessage(
                tr("Already invited %1 recently -- give it a moment.").arg(pending_invite_name));
            pending_invite_pid = 0;
            pending_invite_name.clear();
        } else if (message == QStringLiteral("invite_rate_limited")) {
            chat_room_widget->AppendStatusMessage(
                tr("Sending invites too fast. Try again in a bit."));
            pending_invite_pid = 0;
            pending_invite_name.clear();
        }
    }
}

void NextendoRoomOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath background_path;
    background_path.addRoundedRect(rect(), corner_radius, corner_radius);

    painter.fillPath(background_path, background_color);
    painter.setPen(QPen(border_color, border_width));
    painter.drawPath(background_path);
}

void NextendoRoomOverlay::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!has_been_moved) {
        UpdatePosition();
    }
}

void NextendoRoomOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !size_grip->geometry().contains(event->pos())) {
        if (UISettings::IsGamescope()) {
            is_dragging = true;
            drag_start_pos = event->globalPosition().toPoint() - this->pos();
            setCursor(Qt::ClosedHandCursor);
        } else {
#if defined(Q_OS_LINUX)
            if (windowHandle()) {
                windowHandle()->startSystemMove();
            }
#else
            is_dragging = true;
            drag_start_pos = event->globalPosition().toPoint() - this->pos();
            setCursor(Qt::ClosedHandCursor);
#endif
        }
    }
    QWidget::mousePressEvent(event);
}

void NextendoRoomOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (is_dragging && main_window) {
        const QPoint new_pos = event->globalPosition().toPoint() - drag_start_pos;
        const QPoint win_origin = main_window->mapToGlobal(QPoint(0, 0));
        move(std::clamp(new_pos.x(), win_origin.x(), win_origin.x() + main_window->width() - width()),
             std::clamp(new_pos.y(), win_origin.y(), win_origin.y() + main_window->height() - height()));
    }
    QWidget::mouseMoveEvent(event);
}

void NextendoRoomOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && is_dragging) {
        is_dragging = false;
        has_been_moved = true;
        setCursor(Qt::ArrowCursor);
    }
    QWidget::mouseReleaseEvent(event);
}

void NextendoRoomOverlay::UpdatePosition() {
    if (!main_window || has_been_moved) {
        return;
    }
    // Top-left, mirroring the LDN multiplayer overlay's top-right placement so
    // the two don't stack on top of each other if both are ever visible.
    const QPoint win_pos = main_window->mapToGlobal(QPoint(0, 0));
    move(win_pos.x() + 15, win_pos.y() + 15);
}

void NextendoRoomOverlay::UpdateTheme() {
    if (UISettings::IsDarkTheme()) {
        background_color = QColor(25, 25, 25, 225);
        border_color = QColor(255, 255, 255, 40);
        header_label->setStyleSheet(QStringLiteral("color: #FFFFFF;"));
    } else {
        background_color = QColor(245, 245, 245, 235);
        border_color = QColor(0, 0, 0, 50);
        header_label->setStyleSheet(QStringLiteral("color: #111111;"));
    }
    if (chat_room_widget) {
        chat_room_widget->UpdateTheme();
    }
    update();
}
