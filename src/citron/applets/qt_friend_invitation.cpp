// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include "citron/applets/qt_friend_invitation.h"
#include "common/nextendo_account.h"
#include "core/hle/result.h"
#include "web_service/nextendo_api.h"

namespace {

bool IsHeader(const QListWidgetItem* item) {
    return item->data(Qt::UserRole + 1).toBool();
}

u32 CheckedCount(const QListWidget* list) {
    u32 count = 0;
    for (int i = 0; i < list->count(); ++i) {
        const auto* item = list->item(i);
        if (!IsHeader(item) && item->checkState() == Qt::Checked) ++count;
    }
    return count;
}

// Hides friends whose name doesn't contain text, and section headers left with no visible friend.
void ApplyFilter(QListWidget* list, const QString& text) {
    QListWidgetItem* header = nullptr;
    bool header_has_match = false;
    const auto close_section = [&] {
        if (header) header->setHidden(!header_has_match);
    };
    for (int i = 0; i < list->count(); ++i) {
        auto* item = list->item(i);
        if (IsHeader(item)) {
            close_section();
            header = item;
            header_has_match = false;
            continue;
        }
        const bool match = text.isEmpty() ||
                           item->data(Qt::UserRole + 2).toString().contains(text, Qt::CaseInsensitive);
        item->setHidden(!match);
        header_has_match |= match;
    }
    close_section();
}

} // Anonymous namespace

QtFriendInvitation::QtFriendInvitation(QWidget* parent_) : parent{parent_} {}

QtFriendInvitation::~QtFriendInvitation() {
    if (dialog) dialog->deleteLater();
}

void QtFriendInvitation::Close() const {
    QMetaObject::invokeMethod(const_cast<QtFriendInvitation*>(this), [this] {
        if (dialog) dialog->reject();
    }, Qt::QueuedConnection);
}

void QtFriendInvitation::ShowInvitation(const Core::Frontend::FriendInvitationRequest& request,
                                       u64 title_id, Completion completion_) const {
    QMetaObject::invokeMethod(const_cast<QtFriendInvitation*>(this),
        [this, request, title_id, completion = std::move(completion_)] {
        if (dialog || !parent || request.mode == 10) {
            completion(ResultUnknown.raw);
            return;
        }
        auto* window = new QDialog(parent);
        dialog = window;
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->setWindowTitle(tr("Invite Friends"));
        window->resize(460, 420);
        auto* layout = new QVBoxLayout(window);
        auto* message = new QLabel(tr("Loading your Nextendo friends…"), window);
        message->setWordWrap(true);
        layout->addWidget(message);
        auto* search = new QLineEdit(window);
        search->setPlaceholderText(tr("Search friends"));
        search->setClearButtonEnabled(true);
        layout->addWidget(search);
        auto* friends = new QListWidget(window);
        layout->addWidget(friends);
        connect(search, &QLineEdit::textChanged, friends, [friends](const QString& text) {
            ApplyFilter(friends, text);
        });
        connect(friends, &QListWidget::itemChanged, window,
                [friends, message, limit = request.recipient_limit](QListWidgetItem* item) {
            if (item->checkState() != Qt::Checked || CheckedCount(friends) <= limit) {
                return;
            }
            item->setCheckState(Qt::Unchecked);
            message->setText(tr("You can invite up to %1 friends.").arg(limit));
        });
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, window);
        auto* send = buttons->addButton(tr("Send invitation"), QDialogButtonBox::ActionRole);
        send->setEnabled(false);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, window, &QDialog::reject);
        connect(window, &QDialog::finished, window, [completion](int code) {
            completion(code == QDialog::Accepted ? 0 : ResultUnknown.raw);
        });
        const auto generation = Common::NextendoAccount::GetGeneration();
        auto* load = new QFutureWatcher<WebService::NextendoApi::FriendList>(window);
        connect(load, &QFutureWatcher<WebService::NextendoApi::FriendList>::finished, window,
            [load, friends, message, send, request, generation] {
            const auto result = load->result();
            load->deleteLater();
            if (generation != Common::NextendoAccount::GetGeneration()) {
                message->setText(tr("Your account changed. Close this window and try again.")); return;
            }
            if (!result.ok) { message->setText(QString::fromStdString(result.error)); return; }
            std::vector<WebService::NextendoApi::Friend> online, offline;
            for (const auto& friend_ : result.friends) {
                bool matches = request.mode != 9;
                for (const auto id : request.account_ids) {
                    bool valid{};
                    const auto account_id = QString::fromStdString(friend_.account_hex).toULongLong(&valid, 16);
                    if ((valid && id == account_id) || id == friend_.pid) matches = true;
                }
                if (!matches) continue;
                (friend_.presence_status > 0 ? online : offline).push_back(friend_);
            }
            const auto by_name = [](const auto& a, const auto& b) {
                return QString::fromStdString(a.name).compare(QString::fromStdString(b.name),
                                                              Qt::CaseInsensitive) < 0;
            };
            std::sort(online.begin(), online.end(), by_name);
            std::sort(offline.begin(), offline.end(), by_name);

            const QSignalBlocker blocker(friends);
            const auto add_section = [&](const QString& title, const auto& list, bool is_online) {
                if (list.empty()) return;
                auto* header = new QListWidgetItem(QStringLiteral("%1 — %2").arg(title).arg(list.size()), friends);
                header->setFlags(Qt::NoItemFlags);
                header->setData(Qt::UserRole + 1, true);
                QFont font = header->font();
                font.setBold(true);
                header->setFont(font);
                for (const auto& friend_ : list) {
                    QString label = QString::fromStdString(friend_.name);
                    if (friend_.presence_status == 2 && !friend_.app_name.empty()) {
                        label += tr("  ·  Playing %1").arg(QString::fromStdString(friend_.app_name));
                    }
                    auto* item = new QListWidgetItem(label, friends);
                    item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(friend_.pid));
                    item->setData(Qt::UserRole + 2, QString::fromStdString(friend_.name));
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                    item->setCheckState(request.mode == 9 ? Qt::Checked : Qt::Unchecked);
                    if (!is_online) item->setForeground(friends->palette().color(QPalette::Disabled, QPalette::Text));
                }
            };
            add_section(tr("Online"), online, true);
            add_section(tr("Offline"), offline, false);

            const bool any = !online.empty() || !offline.empty();
            message->setText(any ? tr("Choose up to %1 friends to invite.").arg(request.recipient_limit)
                                 : tr("No friends are available to invite."));
            send->setEnabled(any);
        });
        load->setFuture(QtConcurrent::run([] { return WebService::NextendoApi::GetFriends(); }));
        connect(send, &QPushButton::clicked, window,
            [window, send, friends, message, request, title_id, generation] {
            if (generation != Common::NextendoAccount::GetGeneration()) {
                message->setText(tr("Your account changed. Reopen the invitation window.")); return;
            }
            std::vector<u64> recipients;
            for (int i = 0; i < friends->count(); ++i) {
                auto* item = friends->item(i);
                if (!IsHeader(item) && item->checkState() == Qt::Checked) {
                    recipients.push_back(item->data(Qt::UserRole).toULongLong());
                }
            }
            if (recipients.empty() || recipients.size() > request.recipient_limit) {
                message->setText(tr("Select between 1 and %1 friends.").arg(request.recipient_limit)); return;
            }
            send->setEnabled(false);
            message->setText(tr("Sending invitation…"));
            auto* task = new QFutureWatcher<std::string>(window);
            connect(task, &QFutureWatcher<std::string>::finished, window, [task, window, send, message] {
                const auto error = task->result(); task->deleteLater();
                if (error.empty()) window->accept();
                else { message->setText(QString::fromStdString(error)); send->setEnabled(true); }
            });
            task->setFuture(QtConcurrent::run([title_id, recipients, request] {
                return WebService::NextendoApi::SendGameInvitation(title_id, recipients, request.user_data, request.description);
            }));
        });
        window->open();
    }, Qt::QueuedConnection);
}
