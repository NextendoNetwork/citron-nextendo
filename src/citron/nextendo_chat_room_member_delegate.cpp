// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_chat_room_member_delegate.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include "citron/nextendo_avatar_cache.h"
#include "citron/uisettings.h"

namespace {
QColor NameColor(const QString& name) {
    static constexpr std::array<const char*, 8> palette = {
        "#559AD1", "#4EC9A8", "#D69D85", "#C6C923",
        "#B975B5", "#7EAE39", "#F7CD8A", "#CE4897"};
    uint hash = 0;
    for (const QChar c : name) {
        hash = hash * 31 + static_cast<uint>(c.unicode());
    }
    return QColor(palette[hash % palette.size()]);
}
} // namespace

NextendoChatRoomMemberDelegate::NextendoChatRoomMemberDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

void NextendoChatRoomMemberDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                           const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::TextAntialiasing);

    const bool is_hovered = option.state & QStyle::State_MouseOver;
    const bool is_selected = option.state & QStyle::State_Selected;
    const QRect rect = option.rect;

    const bool is_dark = UISettings::IsDarkTheme();
    const QColor bg_color = is_dark ? QColor("#292929") : QColor("#F4F4F4");

    const QRect safe_rect = rect.adjusted(4, 2, -4, -2);
    if (is_selected) {
        painter->setBrush(bg_color);
        QColor accent = qApp->palette().color(QPalette::Highlight);
        painter->setPen(QPen(accent, 2));
        painter->drawRoundedRect(safe_rect, 6, 6);
    } else if (is_hovered) {
        const QColor hover_color = is_dark ? QColor("#353535") : QColor("#EBEBEB");
        painter->setBrush(hover_color);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(safe_rect, 6, 6);
    } else {
        painter->setBrush(bg_color);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(safe_rect, 6, 6);
    }

    const u64 pid = index.data(NextendoChatMemberItem::PidRole).toULongLong();
    const QString name = index.data(NextendoChatMemberItem::NameRole).toString();
    const QString avatar_b64 = index.data(NextendoChatMemberItem::AvatarB64Role).toString();
    const bool is_host = index.data(NextendoChatMemberItem::IsHostRole).toBool();
    const bool is_muted = index.data(NextendoChatMemberItem::IsMutedRole).toBool();
    const bool is_me = index.data(NextendoChatMemberItem::IsMeRole).toBool();

    const int avatar_x = rect.x() + 8;
    const int avatar_y = rect.y() + (rect.height() - kAvatarSize) / 2;

    // Gold ring for the host, faint outline otherwise.
    if (is_host) {
        painter->setPen(QPen(QColor(0xE0, 0xB0, 0x30), 3));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(avatar_x - 1, avatar_y - 1, kAvatarSize + 2, kAvatarSize + 2);
    } else {
        painter->setPen(QPen(QColor(255, 255, 255, 30), 1));
        painter->drawEllipse(avatar_x, avatar_y, kAvatarSize, kAvatarSize);
    }

    // Fading accent ring while this member has recently spoken, same idea as the
    // LDN member list's highlight animation.
    const float highlight_opacity = index.data(NextendoChatMemberItem::HighlightOpacityRole).toFloat();
    if (highlight_opacity > 0.0f) {
        QColor ring_color = qApp->palette().color(QPalette::Highlight);
        ring_color.setAlphaF(highlight_opacity);
        painter->setPen(QPen(ring_color, 3));
        painter->setBrush(Qt::NoBrush);
        const int pad = is_host ? 4 : 2;
        painter->drawEllipse(avatar_x - pad, avatar_y - pad, kAvatarSize + pad * 2, kAvatarSize + pad * 2);
    }

    const QRect avatar_rect(avatar_x + 2, avatar_y + 2, kAvatarSize - 4, kAvatarSize - 4);
    const QPixmap pixmap =
        Nextendo::AvatarCache::Get(std::to_string(pid), avatar_b64.toStdString(), kAvatarSize - 4);
    if (!pixmap.isNull()) {
        QPainterPath clip;
        clip.addEllipse(avatar_rect);
        painter->save();
        painter->setClipPath(clip);
        painter->drawPixmap(avatar_rect, pixmap);
        painter->restore();
    } else {
        const QColor color = NameColor(name);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawEllipse(avatar_rect);

        const QString initial = name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper();
        QFont avatar_font = painter->font();
        avatar_font.setBold(true);
        avatar_font.setPointSize(16);
        painter->setFont(avatar_font);
        const int brightness =
            (color.red() * 299 + color.green() * 587 + color.blue() * 114) / 1000;
        painter->setPen(brightness > 125 ? Qt::black : Qt::white);
        painter->drawText(avatar_rect, Qt::AlignCenter, initial);
    }

    const int text_x = avatar_x + kAvatarSize + 12;
    int max_text_width = rect.width() - text_x - 8;

    QFont bold_font = painter->font();
    bold_font.setBold(true);
    bold_font.setPointSize(10);
    QColor name_color = is_dark ? QColor("#EAEAEA") : QColor("#222222");
    painter->setFont(bold_font);
    painter->setPen(name_color);

    QString display_name = name;
    if (is_me) {
        display_name += QObject::tr(" (You)");
    }

    // Reserve room for badges before eliding the name so long names don't run
    // underneath them.
    int badge_reserved = 0;
    QFont badge_font = painter->font();
    badge_font.setBold(true);
    badge_font.setPointSize(8);
    QFontMetrics badge_fm(badge_font);
    struct Badge {
        QString text;
        QColor color;
    };
    QList<Badge> badges;
    if (is_host) {
        badges.append({QObject::tr("HOST"), QColor(0xE0, 0xB0, 0x30)});
    }
    if (is_muted) {
        badges.append({QObject::tr("MUTED"), QColor(0xD3, 0x40, 0x40)});
    }
    for (const auto& b : badges) {
        badge_reserved += badge_fm.horizontalAdvance(b.text) + 16 + 6;
    }
    max_text_width -= badge_reserved;
    max_text_width = std::max(max_text_width, 20);

    const QRect name_rect(text_x, rect.y() + (rect.height() - 18) / 2, max_text_width, 18);
    painter->drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(display_name, Qt::ElideRight, max_text_width));

    // Badges, right-aligned pills.
    int badge_x = rect.right() - 8;
    painter->setFont(badge_font);
    for (auto it = badges.rbegin(); it != badges.rend(); ++it) {
        const int w = badge_fm.horizontalAdvance(it->text) + 16;
        const QRect pill(badge_x - w, rect.y() + (rect.height() - 18) / 2, w, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(it->color);
        painter->drawRoundedRect(pill, 9, 9);
        painter->setPen(Qt::black);
        painter->drawText(pill, Qt::AlignCenter, it->text);
        badge_x -= w + 6;
    }

    painter->restore();
}

QSize NextendoChatRoomMemberDelegate::sizeHint(const QStyleOptionViewItem& option,
                                               const QModelIndex& index) const {
    return QSize(200, kRowHeight);
}
