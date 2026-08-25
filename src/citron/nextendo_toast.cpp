// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_toast.h"

#include <algorithm>

#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QScreen>
#include <QWindow>

#include "citron/nextendo_avatar_cache.h"
#include "citron/uisettings.h"

namespace {
// Sizes at scale=1.0 (see NextendoToast::ComputeScale) -- a 1920-wide screen or narrower. This is
// the visible card; the widget itself is larger by 2*kShadowMargin so the painted soft shadow has
// room to bleed into without being clipped.
constexpr int kWidth = 312;
constexpr int kHeight = 78;
constexpr int kMargin = 16;      // gap from the screen edge
constexpr int kTopExtra = 10;    // extra drop below kMargin, clear of the toolbar row
constexpr int kShadowMargin = 16;
constexpr int kAvatarSize = 44;
constexpr int kAccentBarWidth = 4;
constexpr int kAutoHideMs = 7000;
constexpr int kFadeMs = 250;
constexpr int kSlideMs = 280;
constexpr int kSlideDistance = 24; // px the card travels in from the screen edge on Show()
} // Anonymous namespace

NextendoToast::NextendoToast(QWidget* main_window_)
    : QWidget(main_window_), main_window(main_window_),
      fade(new QPropertyAnimation(this, "windowOpacity", this)),
      slide(new QPropertyAnimation(this, "pos", this)) {
    // Qt::Tool maps to xdg_toplevel on Wayland, which the client cannot position at all -- the
    // compositor centers it and ignores move(). Qt::ToolTip maps to xdg_popup instead, which
    // Wayland compositors are required to place at the client's requested position.
    setWindowFlags(Qt::FramelessWindowHint | Qt::ToolTip | Qt::WindowStaysOnTopHint |
                  Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    resize(kWidth, kHeight);
    setWindowOpacity(0.0);
    hide();

    auto_hide_timer.setSingleShot(true);
    connect(&auto_hide_timer, &QTimer::timeout, this, &NextendoToast::HideAnimated);

    if (main_window) {
        main_window->installEventFilter(this);
    }
}

NextendoToast::~NextendoToast() = default;

bool NextendoToast::eventFilter(QObject* watched, QEvent* event) {
    // A toast already on screen doesn't hide itself just because main_window got minimized --
    // it's a separate top-level window (Qt::ToolTip), not a child that follows its parent's state.
    if (watched == main_window && event->type() == QEvent::WindowStateChange &&
        main_window->isMinimized()) {
        auto_hide_timer.stop();
        fade->stop();
        setWindowOpacity(0.0);
        hide();
    }
    return QWidget::eventFilter(watched, event);
}

float NextendoToast::ComputeScale() const {
    QScreen* screen = main_window ? main_window->screen() : nullptr;
    if (!screen) {
        return 1.0f;
    }
    // 1.0 up to a 1920-wide screen, growing to 2.0 by 3840-wide (4K); clamped past either end.
    constexpr float kReferenceWidth = 1920.0f;
    constexpr float kMaxScale = 2.0f;
    return std::clamp(screen->geometry().width() / kReferenceWidth, 1.0f, kMaxScale);
}

NextendoToast::Corner NextendoToast::CurrentCorner() const {
    return static_cast<Corner>(std::clamp(
        UISettings::values.nextendo_notification_corner.GetValue(), 0, 3));
}

void NextendoToast::Show(const QString& headline, const QString& detail,
                         const QString& avatar_base64, Kind kind_) {
    if (!UISettings::values.nextendo_notifications_enabled.GetValue()) {
        return;
    }
    if (!main_window) {
        return;
    }
    // isMinimized() alone is unreliable on Wayland (compositors don't report iconified state the
    // way X11 did); windowHandle()->visibility() catches it there. qApp->activeWindow() covers
    // child dialogs (e.g. the Friends list) too, not just main_window.
    const bool minimized = main_window->isMinimized() ||
                           (main_window->windowHandle() &&
                            main_window->windowHandle()->visibility() == QWindow::Minimized);
    if (minimized || !qApp->activeWindow()) {
        return;
    }

    scale = ComputeScale();
    const int shadow_margin = static_cast<int>(kShadowMargin * scale);
    resize(static_cast<int>(kWidth * scale) + 2 * shadow_margin,
          static_cast<int>(kHeight * scale) + 2 * shadow_margin);

    kind = kind_;
    line1 = headline;
    line2 = detail;
    avatar = Nextendo::AvatarCache::Get(headline.toStdString(), avatar_base64.toStdString(),
                                        static_cast<int>(kAvatarSize * scale));

    Reposition();
    const QPoint rest_pos = pos();
    const int slide_dist = static_cast<int>(kSlideDistance * scale);
    const bool from_left = CurrentCorner() == Corner::TopLeft || CurrentCorner() == Corner::BottomLeft;
    move(rest_pos.x() + (from_left ? -slide_dist : slide_dist), rest_pos.y());
    show();
    raise();
    // Some WMs ignore a WA_ShowWithoutActivating window's requested position at map time and
    // auto-place it instead, but honor move() once it's already shown -- reassert it a tick later.
    QTimer::singleShot(0, this, [this, rest_pos] { move(rest_pos); });

    slide->stop();
    slide->setDuration(kSlideMs);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    slide->setStartValue(pos());
    slide->setEndValue(rest_pos);
    slide->start();

    fade->stop();
    fade->setDuration(kFadeMs);
    fade->setStartValue(windowOpacity());
    fade->setEndValue(1.0);
    fade->start();

    auto_hide_timer.start(kAutoHideMs);
    update();
}

void NextendoToast::Reposition() {
    if (!main_window) {
        return;
    }
    const int edge_margin = static_cast<int>(kMargin * scale);
    const int top_extra = static_cast<int>(kTopExtra * scale);
    const int shadow_margin = static_cast<int>(kShadowMargin * scale);
    const int card_w = static_cast<int>(kWidth * scale);
    const int card_h = static_cast<int>(kHeight * scale);
    const QPoint win_pos = main_window->mapToGlobal(QPoint(0, 0));
    const int left_x = win_pos.x() + edge_margin;
    const int right_x = win_pos.x() + main_window->width() - edge_margin - card_w;
    const int top_y = win_pos.y() + edge_margin + top_extra;
    const int bottom_y = win_pos.y() + main_window->height() - edge_margin - card_h;

    int x = right_x;
    int y = top_y;
    switch (CurrentCorner()) {
    case Corner::TopRight:
        x = right_x;
        y = top_y;
        break;
    case Corner::TopLeft:
        x = left_x;
        y = top_y;
        break;
    case Corner::BottomRight:
        x = right_x;
        y = bottom_y;
        break;
    case Corner::BottomLeft:
        x = left_x;
        y = bottom_y;
        break;
    }
    move(x - shadow_margin, y - shadow_margin);
}

void NextendoToast::HideAnimated() {
    fade->stop();
    fade->setDuration(kFadeMs);
    fade->setStartValue(windowOpacity());
    fade->setEndValue(0.0);
    fade->start();
    // fade->finished() is shared with the fade-IN animation (Show()), so a persistent connection
    // there would also fire -- and hide() -- right as a toast finishes appearing. A plain timer
    // tied to this specific fade-out avoids that.
    QTimer::singleShot(kFadeMs, this, &QWidget::hide);
}

void NextendoToast::mousePressEvent(QMouseEvent* event) {
    QWidget::mousePressEvent(event);
    auto_hide_timer.stop();
    emit clicked(kind);
    HideAnimated();
}

void NextendoToast::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                           QPainter::SmoothPixmapTransform);

    const auto S = [this](int base) { return static_cast<int>(base * scale); };
    const int radius = S(14);

    const QRect card = rect().adjusted(S(kShadowMargin), S(kShadowMargin), -S(kShadowMargin),
                                       -S(kShadowMargin));

    // Soft shadow: a poor-man's blur via layered, low-alpha rounded rects growing outward.
    painter.setPen(Qt::NoPen);
    for (int i = S(8); i >= S(1); i -= std::max(S(1), 1)) {
        QPainterPath layer;
        layer.addRoundedRect(card.adjusted(-i, -i + S(2), i, i + S(2)), radius + i, radius + i);
        painter.fillPath(layer, QColor(0, 0, 0, 6));
    }

    QPainterPath bg;
    bg.addRoundedRect(card, radius, radius);
    const QColor bg_color =
        UISettings::IsDarkTheme() ? QColor(32, 32, 37, 250) : QColor(250, 250, 252, 250);
    painter.fillPath(bg, bg_color);
    const QColor accent = AccentColor();
    painter.setPen(QPen(accent, S(2)));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(bg);

    // Accent bar on the left edge, color-coded per Kind.
    QPainterPath bar;
    bar.addRoundedRect(QRectF(card.left(), card.top(), S(kAccentBarWidth), card.height()),
                       S(2), S(2));
    painter.fillPath(bar.intersected(bg), accent);

    const int avatar_size = S(kAvatarSize);
    const QRect avatar_rect(card.left() + S(20), card.top() + (card.height() - avatar_size) / 2,
                            avatar_size, avatar_size);

    painter.setPen(QPen(accent, S(2)));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(avatar_rect.adjusted(-S(2), -S(2), S(2), S(2)));

    painter.save();
    QPainterPath clip;
    clip.addEllipse(avatar_rect);
    painter.setClipPath(clip);
    if (!avatar.isNull()) {
        painter.drawPixmap(avatar_rect, avatar);
    } else {
        painter.fillRect(avatar_rect, accent);
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPointSizeF(std::max((f.pointSizeF() + 2) * scale, S(12) * 1.0));
        painter.setFont(f);
        painter.setPen(Qt::white);
        painter.drawText(avatar_rect, Qt::AlignCenter,
                         line1.isEmpty() ? QStringLiteral("?") : line1.left(1).toUpper());
    }
    painter.restore();

    const int text_left = avatar_rect.right() + S(18);
    const int text_width = card.right() - S(16) - text_left;

    QFont category_font = QApplication::font();
    category_font.setBold(true);
    category_font.setPointSizeF(std::max((category_font.pointSizeF() - 2) * scale, 6.5 * scale));
    category_font.setLetterSpacing(QFont::AbsoluteSpacing, S(1));
    QFont name_font = QApplication::font();
    name_font.setBold(true);
    name_font.setPointSizeF((name_font.pointSizeF() + 1) * scale);
    QFont sub_font = QApplication::font();
    sub_font.setPointSizeF(std::max(sub_font.pointSizeF() * scale, 7.5 * scale));

    // Category + name + subtitle, centered as a unit rather than stretched to fill the card.
    const QFontMetrics category_fm(category_font);
    const QFontMetrics name_fm(name_font);
    const QFontMetrics sub_fm(sub_font);
    const int line_gap = S(2);
    const int block_h = category_fm.height() + line_gap + name_fm.height() + line_gap + sub_fm.height();
    int block_top = card.top() + (card.height() - block_h) / 2;

    painter.setFont(category_font);
    painter.setPen(accent);
    painter.drawText(QRect(text_left, block_top, text_width, category_fm.height()),
                     Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, CategoryLabel());
    block_top += category_fm.height() + line_gap;

    painter.setFont(name_font);
    painter.setPen(UISettings::IsDarkTheme() ? QColor(240, 240, 243) : QColor(15, 15, 20));
    painter.drawText(QRect(text_left, block_top, text_width, name_fm.height()),
                     Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                     name_fm.elidedText(line1, Qt::ElideRight, text_width));
    block_top += name_fm.height() + line_gap;

    painter.setFont(sub_font);
    painter.setPen(UISettings::IsDarkTheme() ? QColor(180, 180, 187) : QColor(90, 90, 100));
    painter.drawText(QRect(text_left, block_top, text_width, sub_fm.height()),
                     Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                     sub_fm.elidedText(line2, Qt::ElideRight, text_width));
}

QColor NextendoToast::AccentColor() const {
    switch (kind) {
    case Kind::Online:
        return QColor(88, 199, 122); // green -- came online
    case Kind::Offline:
        return QColor(148, 152, 161); // muted gray -- went offline
    case Kind::Request:
    case Kind::ChatRequest:
        return QColor(180, 120, 240); // purple -- incoming request, wants attention
    case Kind::RequestSent:
        return QColor(100, 149, 237); // blue -- confirmation of your own action
    }
    return QColor(100, 149, 237);
}

QString NextendoToast::CategoryLabel() const {
    switch (kind) {
    case Kind::Online:
        return tr("FRIEND ONLINE");
    case Kind::Offline:
        return tr("FRIEND OFFLINE");
    case Kind::Request:
        return tr("FRIEND REQUEST");
    case Kind::RequestSent:
        return tr("REQUEST SENT");
    case Kind::ChatRequest:
        return tr("CHAT INVITE");
    }
    return {};
}
