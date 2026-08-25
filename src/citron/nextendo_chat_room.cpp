// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <regex>
#include <thread>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QColorDialog>
#include <QDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTime>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "citron/nextendo_avatar_cache.h"
#include "citron/nextendo_chat_client.h"
#include "citron/nextendo_chat_room.h"
#include "citron/nextendo_chat_room_member_delegate.h"
#include "citron/theme.h"
#include "citron/uisettings.h"
#include "ui_nextendo_chat_room.h"
#include "web_service/nextendo_api.h"

namespace {
QColor PlayerColorForPid(u64 pid) {
    const bool is_dark = UISettings::IsDarkTheme();
    static constexpr std::array<const char*, 16> default_colors = {
        "#0000FF", "#FF0000", "#8A2BE2", "#FF69B4", "#1E90FF", "#008000", "#00FF7F", "#B22222",
        "#DAA520", "#FF4500", "#2E8B57", "#5F9EA0", "#D2691E", "#9ACD32", "#FF7F50", "#FFFF00"};
    static constexpr std::array<const char*, 16> dark_colors = {
        "#559AD1", "#4EC9A8", "#D69D85", "#C6C923", "#B975B5", "#D81F1F", "#7EAE39", "#4F8733",
        "#F7CD8A", "#6FCACF", "#CE4897", "#8A2BE2", "#D2691E", "#9ACD32", "#FF7F50", "#152ccd"};
    const auto& colors = is_dark ? dark_colors : default_colors;
    return QColor(colors[pid % colors.size()]);
}
} // namespace

class NextendoChatStatusMessage {
public:
    explicit NextendoChatStatusMessage(const QString& msg, QTime ts = {}) {
        QLocale locale;
        timestamp = locale.toString(ts.isValid() ? ts : QTime::currentTime(), QLocale::ShortFormat);
        message = msg;
    }

    QString GetSystemChatMessage(bool show_timestamps) const {
        const QString time_str =
            show_timestamps ? QStringLiteral("[%1] ").arg(timestamp) : QStringLiteral("");
        return QStringLiteral("%1<font color='#FF8C00'>* %2</font>").arg(time_str, message);
    }

private:
    QString timestamp;
    QString message;
};

NextendoChatRoom::NextendoChatRoom(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::NextendoChatRoom>()) {
    ui->setupUi(this);
    setAttribute(Qt::WA_StyledBackground, true);

    ui->player_view->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    QToolButton* emoji_button = new QToolButton(this);
    emoji_button->setText(QStringLiteral("😀"));
    emoji_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    emoji_button->setFixedSize(36, 30);
    emoji_button->setAutoRaise(true);
    emoji_button->setPopupMode(QToolButton::InstantPopup);
    emoji_button->setStyleSheet(QStringLiteral(
        "QToolButton { padding: 0px; margin: 0px; }"
        "QToolButton::menu-indicator { image: none; width: 0px; }"));

    send_message = new QPushButton(QStringLiteral("➤"), this);
    send_message->setObjectName(QStringLiteral("send_message"));
    send_message->setFixedSize(40, 30);
    send_message->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    ui->options_button->setText(QStringLiteral("⚙"));
    ui->horizontalLayout_3->removeWidget(ui->chat_message);
    ui->horizontalLayout_3->addWidget(ui->options_button);
    ui->horizontalLayout_3->addWidget(ui->chat_message);
    ui->horizontalLayout_3->addWidget(emoji_button);
    ui->horizontalLayout_3->addWidget(send_message);
    ui->horizontalLayout_3->setStretch(0, 0);
    ui->horizontalLayout_3->setStretch(1, 1);
    ui->horizontalLayout_3->setStretch(3, 0);

    QMenu* emoji_menu = new QMenu(this);
    QStringList emojis = {
        QStringLiteral("😀"), QStringLiteral("😂"), QStringLiteral("🤣"), QStringLiteral("😊"), QStringLiteral("😎"),
        QStringLiteral("🤔"), QStringLiteral("🙄"), QStringLiteral("🥺"), QStringLiteral("😭"), QStringLiteral("😮"),
        QStringLiteral("🥳"), QStringLiteral("😴"), QStringLiteral("💀"), QStringLiteral("👀"), QStringLiteral("👑"),
        QStringLiteral("👍"), QStringLiteral("👎"), QStringLiteral("👏"), QStringLiteral("🙌"), QStringLiteral("🙏"),
        QStringLiteral("🤝"), QStringLiteral("💪"), QStringLiteral("👋"), QStringLiteral("🎮"), QStringLiteral("🕹️"),
        QStringLiteral("🔥"), QStringLiteral("✨"), QStringLiteral("❤️"), QStringLiteral("🎉"), QStringLiteral("💯"),
        QStringLiteral("🚀"), QStringLiteral("⭐️"), QStringLiteral("💎"), QStringLiteral("📢"), QStringLiteral("🔔"),
        QStringLiteral("✅"), QStringLiteral("❌"), QStringLiteral("🏆"), QStringLiteral("🎧"), QStringLiteral("🫠"),
    };
    QWidget* grid_container = new QWidget(emoji_menu);
    QGridLayout* grid_layout = new QGridLayout(grid_container);
    grid_layout->setSpacing(2);
    grid_layout->setContentsMargins(5, 5, 5, 5);
    const int max_columns = 7;
    for (int i = 0; i < emojis.size(); ++i) {
        const QString emoji = emojis[i];
        QToolButton* btn = new QToolButton(grid_container);
        btn->setText(emoji);
        btn->setFixedSize(32, 30);
        btn->setAutoRaise(true);
        btn->setStyleSheet(QStringLiteral("font-size: 16px;"));
        connect(btn, &QToolButton::clicked, [this, emoji, emoji_menu]() {
            ui->chat_message->insert(emoji);
            ui->chat_message->setFocus();
            emoji_menu->close();
        });
        grid_layout->addWidget(btn, i / max_columns, i % max_columns);
    }
    QWidgetAction* action = new QWidgetAction(emoji_menu);
    action->setDefaultWidget(grid_container);
    emoji_menu->addAction(action);
    emoji_button->setMenu(emoji_menu);

    member_model = new QStandardItemModel(ui->player_view);
    ui->player_view->setModel(member_model);
    ui->player_view->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->player_view->setIndentation(0);
    ui->player_view->setRootIsDecorated(false);
    ui->player_view->setHeaderHidden(true);
    member_model->insertColumns(0, 1);
    member_model->setHeaderData(0, Qt::Horizontal, tr("Members"));
    member_delegate = new NextendoChatRoomMemberDelegate(this);
    ui->player_view->setItemDelegate(member_delegate);

    chat_container = new QWidget(ui->chat_history);
    chat_layout = new QVBoxLayout(chat_container);
    chat_layout->setSpacing(2);
    chat_layout->setContentsMargins(4, 4, 4, 4);
    chat_layout->addStretch(1);
    chat_container->setStyleSheet(QStringLiteral("background: transparent;"));
    ui->chat_history->setWidget(chat_container);
    ui->chat_history->setWidgetResizable(true);
    ui->chat_history->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(ui->player_view, &QTreeView::customContextMenuRequested, this,
            &NextendoChatRoom::PopupContextMenu);
    connect(ui->chat_history, &QWidget::customContextMenuRequested, this,
            &NextendoChatRoom::OnChatContextMenu);
    connect(ui->chat_message, &QLineEdit::returnPressed, this, &NextendoChatRoom::OnSendChat);
    connect(send_message, &QPushButton::clicked, this, &NextendoChatRoom::OnSendChat);
    connect(ui->chat_message, &QLineEdit::textChanged, this, &NextendoChatRoom::OnChatTextChanged);

    UpdateTheme();
}

NextendoChatRoom::~NextendoChatRoom() = default;

void NextendoChatRoom::SetChatClient(NextendoChatClient* client) {
    chat_client = client;
}

void NextendoChatRoom::SetLocalIdentity(u64 pid, const QString& name) {
    my_pid = pid;
    my_name = name;
}

void NextendoChatRoom::SetModPerms(bool is_mod) {
    has_mod_perms = is_mod;
}

void NextendoChatRoom::SetShowOptions(bool show) {
    ui->options_button->setVisible(show);
}

void NextendoChatRoom::Clear() {
    if (chat_layout) {
        QLayoutItem* item;
        while ((item = chat_layout->takeAt(0)) != nullptr) {
            if (item->widget()) {
                delete item->widget();
            }
            delete item;
        }
        chat_layout->addStretch(1);
    }
    block_list.clear();
    member_model->removeRows(0, member_model->rowCount());
}

void NextendoChatRoom::AppendStatusMessage(const QString& msg) {
    if (chat_muted || !chat_layout) {
        return;
    }
    while (chat_layout->count() > static_cast<int>(max_chat_lines)) {
        QLayoutItem* item = chat_layout->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    QWidget* row = new QWidget(chat_container);
    QHBoxLayout* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(4, 2, 4, 2);

    QLabel* message_label =
        new QLabel(NextendoChatStatusMessage(msg).GetSystemChatMessage(show_timestamps));
    message_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    message_label->setWordWrap(true);
    message_label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    message_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row_layout->addWidget(message_label);

    const bool is_dark = UISettings::IsDarkTheme();
    const QString pill_bg =
        is_dark ? QStringLiteral("rgba(50, 50, 55, 60)") : QStringLiteral("rgba(200, 200, 205, 60)");
    row->setStyleSheet(QStringLiteral(
                           "QWidget { background-color: %1; border-left: 2px solid gray; "
                           "border-radius: 4px; padding: 2px 6px; font-style: italic; margin-top: 2px; }"
                           "QLabel { background: none; border: none; }")
                           .arg(pill_bg));

    chat_layout->insertWidget(chat_layout->count() - 1, row);
    row->show();

    QScrollBar* bar = ui->chat_history->verticalScrollBar();
    const bool is_at_bottom = bar->value() == bar->maximum();
    QCoreApplication::processEvents();
    if (is_at_bottom) {
        bar->setValue(bar->maximum());
    }
}

void NextendoChatRoom::AppendChatMessage(const QString& html_msg, u64 sender_pid,
                                         const QColor& color) {
    if (chat_muted || !chat_layout) {
        return;
    }
    if (block_list.count(sender_pid)) {
        return;
    }
    while (chat_layout->count() > static_cast<int>(max_chat_lines)) {
        QLayoutItem* item = chat_layout->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    QWidget* row = new QWidget(chat_container);
    QHBoxLayout* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(4, 2, 4, 2);

    QLabel* message_label = new QLabel(html_msg);
    message_label->setWordWrap(true);
    message_label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    message_label->setOpenExternalLinks(true);
    row_layout->addWidget(message_label);

    const QString bg_color =
        QStringLiteral("rgba(%1, %2, %3, 20)").arg(color.red()).arg(color.green()).arg(color.blue());
    row->setStyleSheet(QStringLiteral(
                           "QWidget { background-color: %1; border-left: 2px solid %2; "
                           "border-radius: 6px; padding: 4px; margin-top: 2px; }"
                           "QLabel { background: none; border: none; padding-left: 4px; padding-right: 4px; }")
                           .arg(bg_color, color.name()));

    chat_layout->insertWidget(chat_layout->count() - 1, row);
    row->show();

    QScrollBar* bar = ui->chat_history->verticalScrollBar();
    const bool is_at_bottom = bar->value() == bar->maximum();
    const bool is_me = sender_pid == my_pid;
    QCoreApplication::processEvents();
    if (is_at_bottom || is_me) {
        bar->setValue(bar->maximum());
    }
}

bool NextendoChatRoom::ValidateMessage(const std::string& msg) {
    return !msg.empty();
}

std::string NextendoChatRoom::SanitizeMessage(const std::string& message) {
    // Ported verbatim from multiplayer/chat_room.cpp's SanitizeMessage.
    std::string sanitized_message = message;
    for (size_t pos = 0; (pos = sanitized_message.find("о", pos)) != std::string::npos;) {
        sanitized_message.replace(pos, 2, "o");
    }
    for (size_t pos = 0; (pos = sanitized_message.find("а", pos)) != std::string::npos;) {
        sanitized_message.replace(pos, 2, "a");
    }
    for (size_t pos = 0; (pos = sanitized_message.find("е", pos)) != std::string::npos;) {
        sanitized_message.replace(pos, 2, "e");
    }
    for (size_t pos = 0; (pos = sanitized_message.find("с", pos)) != std::string::npos;) {
        sanitized_message.replace(pos, 2, "c");
    }
    for (size_t pos = 0; (pos = sanitized_message.find("і", pos)) != std::string::npos;) {
        sanitized_message.replace(pos, 2, "i");
    }

    std::string normalized_message = sanitized_message;
    normalized_message.erase(
        std::remove_if(normalized_message.begin(), normalized_message.end(), ::isspace),
        normalized_message.end());
    std::transform(normalized_message.begin(), normalized_message.end(), normalized_message.begin(),
                   ::tolower);
    normalized_message = std::regex_replace(normalized_message, std::regex("dot|\\(dot\\)|, A T,"), ".");
    normalized_message = std::regex_replace(normalized_message, std::regex("slash|\\(slash\\)"), "/");
    normalized_message = std::regex_replace(normalized_message, std::regex("colon|\\(colon\\)"), ":");

    static const std::regex url_regex(
        R"((?:(?:(?:https?|ftp):\/\/)|www\.|[a-zA-Z0-9-]{1,63}\.(?:com|org|net|gg|dev|io|info|biz|us|ca|uk|de|jp|fr|au|ru|ch|it|nl|se|no|es|mil|edu|gov|ai))\b(?:[-a-zA-Z0-9()@:%_\+.~#?&\/\/=]*))",
        std::regex_constants::icase);
    if (std::regex_search(normalized_message, url_regex)) {
        return "***";
    }
    return message;
}

QColor NextendoChatRoom::GetPlayerColor(u64 pid, const QString& name) const {
    if (color_overrides.count(pid)) {
        return QColor(QString::fromStdString(color_overrides.at(pid)));
    }
    return PlayerColorForPid(pid);
}

QStandardItem* NextendoChatRoom::FindMemberItem(u64 pid) const {
    for (int i = 0; i < member_model->rowCount(); ++i) {
        auto* item = member_model->item(i);
        if (item->data(NextendoChatMemberItem::PidRole).toULongLong() == pid) {
            return item;
        }
    }
    return nullptr;
}

void NextendoChatRoom::FetchAvatar(u64 pid) {
    if (pid == 0 || avatar_cache.count(pid)) {
        return;
    }
    avatar_cache[pid] = QString{};
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, pid, guard = QPointer<NextendoChatRoom>(this)] {
        const std::string b64 = WebService::NextendoApi::GetAvatarByPid(pid);
        if (b64.empty()) {
            return;
        }
        QMetaObject::invokeMethod(
            qApp,
            [this, guard, pid, b64] {
                if (!guard) {
                    return;
                }
                avatar_cache[pid] = QString::fromStdString(b64);
                if (auto* item = FindMemberItem(pid)) {
                    item->setData(avatar_cache[pid], NextendoChatMemberItem::AvatarB64Role);
                }
                ui->player_view->viewport()->update();
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoChatRoom::SetMembers(const QJsonArray& members) {
    member_model->removeRows(0, member_model->rowCount());
    for (const auto& v : members) {
        const auto obj = v.toObject();
        const u64 pid = static_cast<u64>(obj.value(QStringLiteral("pid")).toDouble());
        const QString name = obj.value(QStringLiteral("name")).toString();
        auto* item = new NextendoChatMemberItem(pid, name, obj.value(QStringLiteral("host")).toBool(),
                                                obj.value(QStringLiteral("muted")).toBool(),
                                                pid == my_pid);
        if (avatar_cache.count(pid) && !avatar_cache[pid].isEmpty()) {
            item->setData(avatar_cache[pid], NextendoChatMemberItem::AvatarB64Role);
        }
        member_model->appendRow(item);
        FetchAvatar(pid);
    }
}

void NextendoChatRoom::AddHistoryMessage(u64 pid, const QString& name, const QString& text) {
    OnChatMessage(pid, name, text);
}

void NextendoChatRoom::OnChatMessage(u64 pid, const QString& name, const QString& text) {
    const std::string sanitized = SanitizeMessage(text.toStdString());
    if (!ValidateMessage(sanitized)) {
        return;
    }
    const QString locale_time = QLocale().toString(QTime::currentTime(), QLocale::ShortFormat);
    const QString time_str = show_timestamps
                                 ? QStringLiteral("<span style='color: #999999; font-size: 10px;'>[%1]</span> ")
                                       .arg(locale_time)
                                 : QStringLiteral("");
    const QColor color = GetPlayerColor(pid, name);
    const QString html =
        QStringLiteral("%1<span style='color: %2; font-weight: bold;'>%3</span>&nbsp;&nbsp;<span>%4</span>")
            .arg(time_str, color.name(), name.toHtmlEscaped(),
                 QString::fromStdString(sanitized).toHtmlEscaped());
    AppendChatMessage(html, pid, color);
    HighlightPlayer(pid);
    FetchAvatar(pid);
}

void NextendoChatRoom::OnSendChat() {
    if (!chat_client || !chat_client->IsConnected()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    sent_message_timestamps.erase(
        std::remove_if(sent_message_timestamps.begin(), sent_message_timestamps.end(),
                       [now](const auto& ts) { return (now - ts) > THROTTLE_INTERVAL; }),
        sent_message_timestamps.end());
    if (sent_message_timestamps.size() >= MAX_MESSAGES_PER_INTERVAL) {
        AppendStatusMessage(tr("Spam detected. Please don't send more than 3 messages per every 5 seconds."));
        return;
    }

    const std::string message = SanitizeMessage(ui->chat_message->text().toStdString());
    if (!ValidateMessage(message)) {
        return;
    }

    chat_client->SendJson(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("send_message")},
        {QStringLiteral("text"), QString::fromStdString(message)},
    });
    sent_message_timestamps.push_back(now);
    ui->chat_message->clear();
    // The server echoes our own message back via chat_message, which is what
    // actually renders it -- avoids drawing it twice.
}

void NextendoChatRoom::OnMemberJoined(u64 pid, const QString& name) {
    if (FindMemberItem(pid)) {
        return; // already in the list (e.g. from room_joined's initial snapshot)
    }
    AppendStatusMessage(tr("%1 has joined").arg(name));
    auto* item = new NextendoChatMemberItem(pid, name, /*is_host=*/false, /*is_muted=*/false,
                                            pid == my_pid);
    member_model->appendRow(item);
    FetchAvatar(pid);
}

void NextendoChatRoom::OnMemberLeft(u64 pid) {
    for (int i = 0; i < member_model->rowCount(); ++i) {
        auto* item = member_model->item(i);
        if (item->data(NextendoChatMemberItem::PidRole).toULongLong() == pid) {
            AppendStatusMessage(tr("%1 has left").arg(item->data(NextendoChatMemberItem::NameRole).toString()));
            member_model->removeRow(i);
            break;
        }
    }
}

void NextendoChatRoom::OnMemberUpdated(u64 pid, bool muted) {
    if (auto* item = FindMemberItem(pid)) {
        item->setData(muted, NextendoChatMemberItem::IsMutedRole);
    }
}

void NextendoChatRoom::OnChatTextChanged() {
    static constexpr int kMaxMessageSize = 500;
    if (ui->chat_message->text().length() > kMaxMessageSize) {
        ui->chat_message->setText(ui->chat_message->text().left(kMaxMessageSize));
    }
}

void NextendoChatRoom::PopupContextMenu(const QPoint& menu_location) {
    QMenu context_menu;

    QAction* scroll_action = context_menu.addAction(tr("Hide Member Scrollbar"));
    scroll_action->setCheckable(true);
    scroll_action->setChecked(member_scrollbar_hidden);
    connect(scroll_action, &QAction::triggered, [this](bool checked) {
        member_scrollbar_hidden = checked;
        ui->player_view->setVerticalScrollBarPolicy(checked ? Qt::ScrollBarAlwaysOff
                                                             : Qt::ScrollBarAsNeeded);
    });
    context_menu.addSeparator();

    const QModelIndex item_index = ui->player_view->indexAt(menu_location);
    if (!item_index.isValid()) {
        context_menu.exec(ui->player_view->viewport()->mapToGlobal(menu_location));
        return;
    }

    const u64 pid = member_model->item(item_index.row())->data(NextendoChatMemberItem::PidRole).toULongLong();
    const QString name = member_model->item(item_index.row())->data(NextendoChatMemberItem::NameRole).toString();

    QAction* color_action = context_menu.addAction(tr("Set Name Color"));
    connect(color_action, &QAction::triggered, [this, pid, name] {
        const QColor color = QColorDialog::getColor(Qt::white, this, tr("Select Color for %1").arg(name));
        if (color.isValid()) {
            color_overrides[pid] = color.name().toStdString();
        }
    });

    if (pid != my_pid) {
        QAction* block_action = context_menu.addAction(tr("Block Player"));
        block_action->setCheckable(true);
        block_action->setChecked(block_list.count(pid) > 0);
        connect(block_action, &QAction::triggered, [this, pid] {
            if (block_list.count(pid)) {
                block_list.erase(pid);
            } else {
                block_list.insert(pid);
            }
        });

        QAction* report_action = context_menu.addAction(tr("Report Player..."));
        connect(report_action, &QAction::triggered, [this, pid, name] { ReportPlayer(pid, name); });
    }

    if (has_mod_perms && pid != my_pid && chat_client) {
        const bool currently_muted =
            member_model->item(item_index.row())->data(NextendoChatMemberItem::IsMutedRole).toBool();
        context_menu.addSeparator();
        QAction* kick_action = context_menu.addAction(tr("Kick"));
        QAction* mute_action = context_menu.addAction(currently_muted ? tr("Unmute") : tr("Mute"));
        connect(kick_action, &QAction::triggered, [this, pid] {
            chat_client->SendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("kick")},
                                              {QStringLiteral("target_pid"), static_cast<double>(pid)}});
        });
        connect(mute_action, &QAction::triggered, [this, pid, currently_muted] {
            chat_client->SendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("mute")},
                                              {QStringLiteral("target_pid"), static_cast<double>(pid)},
                                              {QStringLiteral("muted"), !currently_muted}});
        });
    }

    context_menu.exec(ui->player_view->viewport()->mapToGlobal(menu_location));
}

void NextendoChatRoom::OnChatContextMenu(const QPoint& menu_location) {
    QMenu context_menu(this);

    QAction* clear_action = context_menu.addAction(tr("Clear Chat History"));
    connect(clear_action, &QAction::triggered, this, &NextendoChatRoom::Clear);

    QAction* time_action = context_menu.addAction(tr("Show Timestamps"));
    time_action->setCheckable(true);
    time_action->setChecked(show_timestamps);
    connect(time_action, &QAction::triggered, [this](bool checked) { show_timestamps = checked; });

    context_menu.exec(ui->chat_history->viewport()->mapToGlobal(menu_location));
}

void NextendoChatRoom::HighlightPlayer(u64 pid) {
    auto& state = highlight_states[pid];
    if (state.animation) {
        state.animation->stop();
        state.animation->deleteLater();
    }
    if (state.linger_timer) {
        state.linger_timer->stop();
        state.linger_timer->deleteLater();
        state.linger_timer = nullptr;
    }

    auto* fade_in = new QVariantAnimation(this);
    state.animation = fade_in;
    fade_in->setDuration(400);
    fade_in->setStartValue(state.opacity);
    fade_in->setEndValue(1.0f);
    fade_in->setEasingCurve(QEasingCurve::OutQuad);

    connect(fade_in, &QVariantAnimation::valueChanged, [this, pid](const QVariant& value) {
        if (highlight_states.count(pid)) {
            highlight_states[pid].opacity = value.toFloat();
            if (auto* item = FindMemberItem(pid)) {
                item->setData(value.toFloat(), NextendoChatMemberItem::HighlightOpacityRole);
            }
        }
    });
    connect(fade_in, &QVariantAnimation::finished, [this, pid]() {
        if (!highlight_states.count(pid)) {
            return;
        }
        auto& s1 = highlight_states[pid];
        if (s1.animation) {
            s1.animation->deleteLater();
        }
        s1.linger_timer = new QTimer(this);
        s1.linger_timer->setSingleShot(true);
        connect(s1.linger_timer, &QTimer::timeout, [this, pid]() {
            if (!highlight_states.count(pid)) {
                return;
            }
            auto& s2 = highlight_states[pid];
            auto* fade_out = new QVariantAnimation(this);
            s2.animation = fade_out;
            fade_out->setDuration(400);
            fade_out->setStartValue(1.0f);
            fade_out->setEndValue(0.0f);
            fade_out->setEasingCurve(QEasingCurve::OutQuad);
            connect(fade_out, &QVariantAnimation::valueChanged, [this, pid](const QVariant& value) {
                if (highlight_states.count(pid)) {
                    highlight_states[pid].opacity = value.toFloat();
                    if (auto* item = FindMemberItem(pid)) {
                        item->setData(value.toFloat(), NextendoChatMemberItem::HighlightOpacityRole);
                    }
                }
            });
            connect(fade_out, &QVariantAnimation::finished, [this, pid]() {
                if (highlight_states.count(pid)) {
                    auto& final_state = highlight_states[pid];
                    if (final_state.animation) {
                        final_state.animation->deleteLater();
                    }
                    highlight_states.erase(pid);
                }
            });
            fade_out->start();
        });
        s1.linger_timer->start(10000);
    });
    fade_in->start();
}

void NextendoChatRoom::UpdateTheme() {
    QString style_sheet;
    const QString accent_color = Theme::GetAccentColor();
    if (UISettings::IsDarkTheme()) {
        style_sheet = QStringLiteral(R"(
            NextendoChatRoom { background-color: #121212; }
            QWidget#chat_container { background: transparent; }
            QScrollArea { background-color: #1A1A1B; border: 1px solid #282828; border-radius: 8px; }
            QTreeView { background-color: #1A1A1B; color: #E0E0E0; border: 1px solid #282828; border-radius: 8px; outline: none; selection-background-color: transparent; selection-color: inherit; show-decoration-selected: 0; }
            QTreeView::item { margin: 2px; padding: 0px; }
            QTreeView::branch { border-image: none; image: none; }
            QLineEdit { background-color: #202022; color: #EAEAEA; border: 1px solid #282828; padding: 6px 10px; border-radius: 8px; font-size: 13px; }
            QLineEdit:focus { border: 1px solid %1; }
            QPushButton { background-color: #2D2D30; color: #FFFFFF; border: none; padding: 4px; border-radius: 6px; }
            QPushButton:hover { background-color: #38383B; }
            QPushButton#send_message { padding: 0px; margin: 0px; min-width: 40px; max-width: 40px; }
            QToolButton { padding: 4px; margin: 0px; font-size: 14px; border: none; border-radius: 4px; background: #2D2D30; color: #E0E0E0; }
            QToolButton:hover { background: #38383B; }
            QToolButton::menu-indicator { image: none; }
        )").arg(accent_color);
    } else {
        style_sheet = QStringLiteral(R"(
            NextendoChatRoom { background-color: #F0F0F0; }
            QWidget#chat_container { background: transparent; }
            QScrollArea { background-color: #FAFAFA; border: 1px solid #E0E0E0; border-radius: 8px; }
            QTreeView { background-color: #FAFAFA; color: #000000; border: 1px solid #E0E0E0; border-radius: 8px; outline: none; selection-background-color: transparent; selection-color: inherit; show-decoration-selected: 0; }
            QTreeView::item { margin: 2px; padding: 0px; }
            QTreeView::branch { border-image: none; image: none; }
            QLineEdit { background-color: #FFFFFF; color: #000000; border: 1px solid #DFDFDF; padding: 6px 10px; border-radius: 8px; font-size: 13px; }
            QLineEdit:focus { border: 1px solid %1; }
            QPushButton { background-color: #E2E2E2; color: #000000; border: none; padding: 4px; border-radius: 6px; }
            QPushButton:hover { background-color: #D5D5D5; }
            QPushButton#send_message { padding: 0px; margin: 0px; min-width: 40px; max-width: 40px; }
            QToolButton { padding: 4px; margin: 0px; font-size: 14px; border: none; border-radius: 4px; background: #E2E2E2; color: #000000; }
            QToolButton:hover { background: #D5D5D5; }
            QToolButton::menu-indicator { image: none; }
        )").arg(accent_color);
    }
    this->setStyleSheet(style_sheet);
}

void NextendoChatRoom::ReportPlayer(u64 pid, const QString& name) {
#ifdef ENABLE_WEB_SERVICE
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Report %1").arg(name));

    auto* reason_combo = new QComboBox(&dialog);
    reason_combo->addItem(tr("Cheating"), QStringLiteral("cheating"));
    reason_combo->addItem(tr("Inappropriate name"), QStringLiteral("name"));
    reason_combo->addItem(tr("Impersonating someone else's in-game name"),
                          QStringLiteral("name_mismatch"));
    reason_combo->addItem(tr("Inappropriate avatar"), QStringLiteral("avatar"));
    reason_combo->addItem(tr("Harassment"), QStringLiteral("harassment"));
    reason_combo->addItem(tr("Griefing"), QStringLiteral("griefing"));
    reason_combo->addItem(tr("Impersonating a Nextendo account"), QStringLiteral("impersonation"));
    reason_combo->addItem(tr("Other"), QStringLiteral("other"));

    auto* comment_edit = new QLineEdit(&dialog);
    comment_edit->setPlaceholderText(tr("What happened? (optional)"));

    auto* buttons = new QHBoxLayout;
    auto* cancel_button = new QPushButton(tr("Cancel"), &dialog);
    auto* send_button = new QPushButton(tr("Send Report"), &dialog);
    send_button->setDefault(true);
    buttons->addStretch(1);
    buttons->addWidget(cancel_button);
    buttons->addWidget(send_button);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Reason:"), &dialog));
    layout->addWidget(reason_combo);
    layout->addWidget(comment_edit);
    layout->addLayout(buttons);

    connect(cancel_button, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(send_button, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const std::string reason = reason_combo->currentData().toString().toStdString();
    const std::string comment = comment_edit->text().toStdString();

    std::thread{[pid, reason, comment, guard = QPointer<NextendoChatRoom>(this)] {
        const std::string error = WebService::NextendoApi::ReportPlayer(pid, reason, comment);
        QMetaObject::invokeMethod(
            qApp,
            [guard, error] {
                if (!guard) {
                    return;
                }
                if (error.empty()) {
                    guard->AppendStatusMessage(tr("Report sent. Thanks for helping keep things clean."));
                } else {
                    QMessageBox::warning(guard, tr("Report Player"),
                                         tr("Couldn't send the report: %1")
                                             .arg(QString::fromStdString(error)));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}
