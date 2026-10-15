// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_friend_delegate.h"

#include <algorithm>

#include <QApplication>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QListView>
#include <QModelIndex>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "citron/nextendo_account_page_p.h"
#include "citron/nextendo_avatar_cache.h"
#include "citron/uisettings.h"

namespace {
QRect CardRect(const QRect& cell_rect) {
    return cell_rect.adjusted(4, NextendoFriendDelegate::kCardMarginV, -4,
                              -NextendoFriendDelegate::kCardMarginV);
}
} // Anonymous namespace

NextendoFriendDelegate::NextendoFriendDelegate(QListView* view, QObject* parent)
    : QStyledItemDelegate(parent), list_view(view), anim_timer(new QTimer(this)) {
    anim_timer->setInterval(40);
    connect(anim_timer, &QTimer::timeout, this, &NextendoFriendDelegate::AdvanceAnimations);
    anim_timer->start();
}

NextendoFriendDelegate::~NextendoFriendDelegate() = default;

void NextendoFriendDelegate::initStyleOption(QStyleOptionViewItem* option,
                                             const QModelIndex& index) const {
    QStyledItemDelegate::initStyleOption(option, index);
    option->state &= ~(QStyle::State_Selected | QStyle::State_HasFocus | QStyle::State_Active |
                       QStyle::State_Sunken);
    option->showDecorationSelected = false;
}

QSize NextendoFriendDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return QSize(0, kCardHeight);
}

void NextendoFriendDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                   const QModelIndex& index) const {
    painter->save();
    painter->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                            QPainter::SmoothPixmapTransform);

    PaintBackground(painter, option.rect, index);

    const QRect card = CardRect(option.rect);
    const int avatar_left = card.left() + 8;
    const QRect avatar_rect(avatar_left, card.top() + (card.height() - kAvatarSize) / 2,
                            kAvatarSize, kAvatarSize);
    PaintAvatar(painter, avatar_rect, index);

    const bool is_request = index.data(NextendoFriendItem::IsRequestRole).toBool();
    const bool is_me = index.data(NextendoFriendItem::IsMeRole).toBool();
    const QString pill_label = index.data(NextendoFriendItem::PillLabelRole).toString();
    const QRect actions = ActionsRect(card);
    const QRect text_rect(avatar_rect.right() + 10, card.top(), actions.left() - 6 - avatar_rect.right() - 10,
                          card.height());
    PaintNameAndStatus(painter, text_rect, option, index);
    if (!is_me) {
        const qreal hov = hover_prog.value(QPersistentModelIndex(index), 0.0);
        PaintActions(painter, card, is_request, pill_label, hov);
    }

    if (list_view && list_view->hasFocus() && list_view->currentIndex() == index) {
        painter->setPen(QPen(AccentColor(), 2));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(card.adjusted(1, 1, -1, -1), kCardRadius, kCardRadius);
    }

    painter->restore();
}

QRect NextendoFriendDelegate::ActionsRect(const QRect& card_rect) const {
    constexpr int width = 132;
    constexpr int right_margin = 14;
    return QRect(card_rect.right() - right_margin - width, card_rect.top(), width,
                card_rect.height());
}

NextendoFriendDelegate::ActionHit NextendoFriendDelegate::HitTestActions(const QRect& cell_rect,
                                                                         const QPoint& pos,
                                                                         bool is_request) const {
    const QRect card = CardRect(cell_rect);
    const QRect actions = ActionsRect(card);
    if (!actions.contains(pos)) {
        return ActionHit::None;
    }

    if (!is_request) {
        // Single centered "Remove" pill occupying the right half of the actions rect.
        const QRect pill(actions.right() - 64, actions.top() + (actions.height() - 24) / 2, 64, 24);
        return pill.contains(pos) ? ActionHit::Secondary : ActionHit::None;
    }

    const int pill_w = 60;
    const QRect primary(actions.left(), actions.top() + (actions.height() - 24) / 2, pill_w, 24);
    const QRect secondary(actions.left() + pill_w + 4, actions.top() + (actions.height() - 24) / 2,
                          pill_w, 24);
    if (primary.contains(pos)) {
        return ActionHit::Primary;
    }
    if (secondary.contains(pos)) {
        return ActionHit::Secondary;
    }
    return ActionHit::None;
}

void NextendoFriendDelegate::AdvanceAnimations() {
    if (!list_view) {
        return;
    }
    bool dirty = false;

    const QPoint cursor_pos = QCursor::pos();
    const bool mouse_moved = cursor_pos != last_cursor_pos;
    last_cursor_pos = cursor_pos;

    // Whichever input actually moved most recently owns the highlight; a resting mouse (the
    // normal state while hovering) must not hand it back to a stale controller selection just
    // because it didn't move on this exact 40ms tick.
    const QModelIndex current = list_view->currentIndex();
    const bool controller_moved =
        list_view->hasFocus() && current.isValid() && current != last_controller_index;
    last_controller_index = current;
    if (mouse_moved) {
        mouse_is_driving = true;
    } else if (controller_moved) {
        mouse_is_driving = false;
    }

    const QPoint mp = list_view->viewport()->mapFromGlobal(cursor_pos);
    const QModelIndex hov_raw = list_view->indexAt(mp);

    QPersistentModelIndex hov_key;
    if (mouse_is_driving && hov_raw.isValid()) {
        hov_key = QPersistentModelIndex(hov_raw);
    } else if (list_view->hasFocus() && current.isValid()) {
        hov_key = QPersistentModelIndex(current);
    } else if (hov_raw.isValid()) {
        hov_key = QPersistentModelIndex(hov_raw);
    }

    if (hov_key.isValid() && !hover_prog.contains(hov_key)) {
        hover_prog[hov_key] = 0.0;
    }

    for (auto it = hover_prog.begin(); it != hover_prog.end();) {
        const bool hov = it.key() == hov_key;
        qreal& p = it.value();
        p = hov ? std::min(p + 0.12, 1.0) : std::max(p - 0.12, 0.0);
        dirty = true;
        it = (p <= 0.0 && !hov) ? hover_prog.erase(it) : std::next(it);
    }

    if (dirty) {
        list_view->viewport()->update();
    }
}

void NextendoFriendDelegate::PaintBackground(QPainter* painter, const QRect& cell_rect,
                                             const QModelIndex& index) const {
    const QRect card = CardRect(cell_rect);
    const qreal hov = hover_prog.value(QPersistentModelIndex(index), 0.0);

    QColor bg = CardBg();
    if (hov > 0.0) {
        bg = bg.lighter(100 + static_cast<int>(hov * 14));
    }

    QPainterPath path;
    path.addRoundedRect(card, kCardRadius, kCardRadius);
    painter->fillPath(path, bg);
}

void NextendoFriendDelegate::PaintAvatar(QPainter* painter, const QRect& r,
                                         const QModelIndex& index) const {
    const u64 pid = index.data(NextendoFriendItem::PidRole).toULongLong();
    const std::string avatar_b64 = index.data(NextendoFriendItem::AvatarB64Role).toString().toStdString();
    QPixmap pixmap = Nextendo::AvatarCache::Get(std::to_string(pid), avatar_b64, kAvatarSize);

    painter->save();
    QPainterPath clip;
    clip.addEllipse(r);
    painter->setClipPath(clip);

    if (!pixmap.isNull()) {
        painter->drawPixmap(r, pixmap);
    } else {
        const QString name = index.data(NextendoFriendItem::NameRole).toString();
        const QChar initial = name.isEmpty() ? QChar(QLatin1Char('?')) : name.at(0).toUpper();
        painter->fillRect(r, AccentColor().darker(120));
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPointSize(std::max(f.pointSize() + 2, 10));
        painter->setFont(f);
        painter->setPen(Qt::white);
        painter->drawText(r, Qt::AlignCenter, initial);
    }
    painter->restore();

    // Presence dot, bottom-right of the avatar.
    const int status = index.data(NextendoFriendItem::PresenceRole).toInt();
    const int dot = 12;
    const QRect dot_rect(r.right() - dot + 2, r.bottom() - dot + 2, dot, dot);
    painter->save();
    painter->setPen(QPen(CardBg(), 2));
    painter->setBrush(PresenceColor(status));
    painter->drawEllipse(dot_rect);
    painter->restore();
}

void NextendoFriendDelegate::PaintNameAndStatus(QPainter* painter, const QRect& r,
                                                const QStyleOptionViewItem& option,
                                                const QModelIndex& index) const {
    if (r.width() <= 0) {
        return;
    }

    const QString name = index.data(NextendoFriendItem::NameRole).toString();
    const int status = index.data(NextendoFriendItem::PresenceRole).toInt();
    const QString game = index.data(NextendoFriendItem::GamePresenceRole).toString();

    QString status_text;
    switch (status) {
    case 1:
        status_text = tr("Online");
        break;
    case 2:
        status_text = game.isEmpty() ? tr("In a game") : tr("Playing %1").arg(game);
        break;
    default:
        status_text = tr("Offline");
        break;
    }
    if (const QString platform = index.data(NextendoFriendItem::PlatformRole).toString();
        status > 0 && !platform.isEmpty()) {
        status_text += QStringLiteral(" · ") + platform;
    }
    if (const QString detail = index.data(NextendoFriendItem::DetailRole).toString();
        !detail.isEmpty()) {
        status_text = detail;
    }

    const int half_h = r.height() / 2;
    QFont name_font = option.font;
    name_font.setBold(true);
    painter->setFont(name_font);
    painter->setPen(FgColor());
    painter->drawText(QRect(r.left(), r.top(), r.width(), half_h),
                      Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                      QFontMetrics(name_font).elidedText(name, Qt::ElideRight, r.width()));

    QFont sub_font = option.font;
    sub_font.setPointSize(std::max(sub_font.pointSize() - 1, 7));
    painter->setFont(sub_font);
    painter->setPen(PresenceColor(status));
    painter->drawText(QRect(r.left(), r.top() + half_h, r.width(), r.height() - half_h),
                      Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                      QFontMetrics(sub_font).elidedText(status_text, Qt::ElideRight, r.width()));
}

void NextendoFriendDelegate::PaintActions(QPainter* painter, const QRect& card_rect,
                                          bool is_request, const QString& pill_label,
                                          qreal hover) const {
    const QRect actions = ActionsRect(card_rect);

    painter->save();
    QFont f = QApplication::font();
    f.setPointSize(std::max(f.pointSize() - 1, 7));
    f.setBold(true);
    painter->setFont(f);

    const qreal intensity = 0.4 + 0.6 * hover;
    auto draw_pill = [&](const QRect& pill, const QString& text, const QColor& color) {
        QPainterPath path;
        path.addRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
        QColor fill = color;
        fill.setAlpha(static_cast<int>(38 * intensity));
        painter->fillPath(path, fill);
        QColor outline = color;
        outline.setAlphaF(intensity);
        painter->setPen(QPen(outline, 1.2));
        painter->drawPath(path);
        painter->setPen(outline);
        painter->drawText(pill, Qt::AlignCenter, text);
    };

    if (is_request) {
        const int pill_w = 60;
        const QRect primary(actions.left(), actions.top() + (actions.height() - 24) / 2, pill_w, 24);
        const QRect secondary(actions.left() + pill_w + 4, actions.top() + (actions.height() - 24) / 2,
                              pill_w, 24);
        draw_pill(primary, tr("Accept"), QColor(50, 195, 85));
        draw_pill(secondary, tr("Decline"), QColor(220, 80, 70));
    } else {
        const QRect pill(actions.right() - 64, actions.top() + (actions.height() - 24) / 2, 64, 24);
        draw_pill(pill, pill_label.isEmpty() ? tr("Remove") : pill_label, QColor(220, 80, 70));
    }

    painter->restore();
}

QColor NextendoFriendDelegate::CardBg() const {
    return UISettings::IsDarkTheme() ? QColor(36, 36, 40) : QColor(240, 240, 244);
}

QColor NextendoFriendDelegate::FgColor() const {
    return UISettings::IsDarkTheme() ? QColor(230, 230, 234) : QColor(22, 22, 28);
}

QColor NextendoFriendDelegate::DimColor() const {
    return UISettings::IsDarkTheme() ? QColor(145, 145, 155) : QColor(100, 100, 112);
}

QColor NextendoFriendDelegate::AccentColor() const {
    const QString hex = QString::fromStdString(UISettings::values.accent_color.GetValue());
    if (QColor(hex).isValid()) {
        return QColor(hex);
    }
    const QColor pa = QApplication::palette().color(QPalette::Highlight);
    return (pa.isValid() && pa != Qt::black) ? pa : QColor(100, 149, 237);
}

QColor NextendoFriendDelegate::PresenceColor(int status) const {
    switch (status) {
    case 1:
        return QColor(100, 149, 237); // online, no game
    case 2:
        return QColor(50, 195, 85); // playing a game
    default:
        return QColor(120, 120, 120); // offline
    }
}
