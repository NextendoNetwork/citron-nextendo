// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFont>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "common/fs/path_util.h"
#include "common/nextendo_account.h"
#include "common/nextendo_outgoing_requests.h"
#include "common/settings.h"
#include "citron/nextendo_account_dialog.h"
#include "citron/nextendo_account_page_p.h"
#include "citron/nextendo_avatar_cache.h"
#include "common/nextendo_compatible_titles.h"
#include "citron/nextendo_controller.h"
#include "citron/nextendo_friend_delegate.h"
#include "citron/nextendo_history_delegate.h"
#include "citron/nextendo_network_probe.h"
#include "citron/uisettings.h"
#include "citron/util/controller_navigation.h"
#include "core/core.h"
#include "hid_core/hid_core.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace {

constexpr int kHeaderAvatarSize = 72;
constexpr int kPreviewAvatarSize = 30;

QColor CardBg() {
    return UISettings::IsDarkTheme() ? QColor(30, 30, 34) : QColor(244, 244, 248);
}

QColor DimColor() {
    return UISettings::IsDarkTheme() ? QColor(150, 150, 158) : QColor(110, 110, 120);
}

QColor AccentColor() {
    const QString hex = QString::fromStdString(UISettings::values.accent_color.GetValue());
    if (QColor(hex).isValid()) {
        return QColor(hex);
    }
    const QColor pa = QApplication::palette().color(QPalette::Highlight);
    return (pa.isValid() && pa != Qt::black) ? pa : QColor(100, 149, 237);
}

QColor PresenceColor(int status) {
    switch (status) {
    case 1:
        return QColor(100, 149, 237);
    case 2:
        return QColor(50, 195, 85);
    default:
        return QColor(120, 120, 120);
    }
}

QPixmap RoundedPixmap(const QPixmap& source, int size) {
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap out(qMax(1, static_cast<int>(size * dpr)), qMax(1, static_cast<int>(size * dpr)));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    painter.setClipPath(clip);
    if (source.isNull()) {
        painter.fillRect(0, 0, size, size, QColor(100, 149, 237));
    } else {
        painter.drawPixmap(0, 0, size, size, source);
    }
    return out;
}

QPixmap RoundedRectPixmap(const QPixmap& source, int size, int radius) {
    if (source.isNull()) {
        return {};
    }
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap out(qMax(1, static_cast<int>(size * dpr)), qMax(1, static_cast<int>(size * dpr)));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(0, 0, size, size, radius, radius);
    painter.setClipPath(clip);
    painter.drawPixmap(0, 0, size, size, source);
    return out;
}

std::filesystem::path BackgroundImagePath() {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir) /
           "nextendo_profile_background.png";
}

class HeaderCard : public QWidget {
public:
    explicit HeaderCard(QWidget* parent) : QWidget(parent) {
        auto* timer = new QTimer(this);
        timer->setInterval(33);
        connect(timer, &QTimer::timeout, this, [this] {
            phase += 0.035;
            update();
        });
        timer->start();
    }

    void SetBackgroundImage(const QPixmap& pixmap) {
        background = pixmap;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRect card = rect().adjusted(1, 1, -1, -1);
        QPainterPath clip_path;
        clip_path.addRoundedRect(card, 12, 12);
        painter.setClipPath(clip_path);
        painter.fillPath(clip_path, CardBg());

        if (!background.isNull()) {
            const QSize scaled_size = background.size().scaled(card.size(), Qt::KeepAspectRatioByExpanding);
            const QPixmap scaled = background.scaled(scaled_size, Qt::KeepAspectRatioByExpanding,
                                                      Qt::SmoothTransformation);
            const QPoint offset((card.width() - scaled.width()) / 2,
                                (card.height() - scaled.height()) / 2);
            painter.drawPixmap(card.topLeft() + offset, scaled);

            QLinearGradient scrim(card.left(), 0, card.right(), 0);
            scrim.setColorAt(0.0, QColor(0, 0, 0, 150));
            scrim.setColorAt(0.55, QColor(0, 0, 0, 80));
            scrim.setColorAt(1.0, QColor(0, 0, 0, 40));
            painter.fillRect(card, scrim);
        }

        const QColor accent = AccentColor();
        const qreal breathe = 0.5 + 0.5 * std::sin(phase);

        const int glow_w = std::min(100, card.width() / 3);
        QColor edge = accent;
        edge.setAlphaF(0.22 * breathe);
        QColor edge_fade = accent;
        edge_fade.setAlphaF(0.0);

        QLinearGradient left_glow(card.left(), 0, card.left() + glow_w, 0);
        left_glow.setColorAt(0.0, edge);
        left_glow.setColorAt(1.0, edge_fade);
        painter.fillRect(QRect(card.left(), card.top(), glow_w, card.height()), left_glow);

        QLinearGradient right_glow(card.right() - glow_w, 0, card.right(), 0);
        right_glow.setColorAt(0.0, edge_fade);
        right_glow.setColorAt(1.0, edge);
        painter.fillRect(QRect(card.right() - glow_w, card.top(), glow_w, card.height()), right_glow);

        if (background.isNull()) {
            struct WaveBand {
                qreal base_frac, amp_frac, phase_off, speed, shade;
                int alpha;
            };
            static constexpr WaveBand kBands[] = {
                {0.55, 0.05, 0.0, 1.5, 0.28, 70},   {0.42, 0.07, 1.1, 1.2, -0.30, 60},
                {0.70, 0.035, 2.3, 0.9, 0.50, 50},  {0.30, 0.09, 0.6, 1.7, -0.42, 42},
                {0.85, 0.03, 3.5, 0.7, 0.66, 34},
            };
            const auto shade = [](QColor c, qreal f) {
                const auto adj = [f](int v) {
                    return f >= 0.0 ? static_cast<int>(v + (255 - v) * f) : static_cast<int>(v * (1.0 + f));
                };
                return QColor(adj(c.red()), adj(c.green()), adj(c.blue()));
            };
            constexpr int kSteps = 48;
            for (const auto& band : kBands) {
                const qreal base_y = card.top() + card.height() * band.base_frac;
                const qreal amp = card.height() * band.amp_frac;
                const qreal ph = phase * band.speed + band.phase_off;

                QPainterPath fill;
                fill.moveTo(card.left(), card.bottom());
                for (int s = 0; s <= kSteps; ++s) {
                    const qreal nx = static_cast<qreal>(s) / kSteps;
                    const qreal x = card.left() + nx * card.width();
                    const qreal y = base_y + std::sin(nx * 2 * M_PI + ph) * amp +
                                   std::cos(nx * 2 * M_PI * 1.7 + ph * 0.8) * amp * 0.4;
                    fill.lineTo(x, y);
                }
                fill.lineTo(card.right(), card.bottom());
                fill.closeSubpath();

                QColor band_color = shade(accent, band.shade);
                band_color.setAlpha(band.alpha);
                painter.fillPath(fill, band_color);
            }
        }

        painter.setClipping(false);
        painter.setPen(QPen(QColor(255, 255, 255, 20), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(clip_path);
    }

private:
    qreal phase = 0.0;
    QPixmap background;
};

class ClickableFrame : public QFrame {
public:
    explicit ClickableFrame(QWidget* parent = nullptr) : QFrame(parent) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    std::function<void()> on_click;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && on_click && rect().contains(event->pos())) {
            on_click();
        }
        QFrame::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter) &&
            on_click) {
            on_click();
            return;
        }
        QFrame::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        QFrame::paintEvent(event);
        if (!hasFocus()) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(AccentColor(), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 10, 10);
    }
};

QString DashCardStyle() {
    return QStringLiteral("QFrame { background: %1; border-radius: 12px; }").arg(CardBg().name());
}

QFrame* MakeDashCard() {
    auto* card = new QFrame;
    card->setStyleSheet(DashCardStyle());
    return card;
}

QLabel* MakeCardTitle(const QString& text) {
    auto* label = new QLabel(text);
    QFont f = label->font();
    f.setBold(true);
    f.setPointSize(f.pointSize() + 1);
    label->setFont(f);
    return label;
}

QLabel* MakeDimLabel(const QString& text) {
    auto* label = new QLabel(text);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, DimColor());
    label->setPalette(pal);
    return label;
}

QListView* MakeCardList(QWidget* parent) {
    auto* view = new QListView(parent);
    view->setSelectionMode(QAbstractItemView::NoSelection);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setFrameShape(QFrame::NoFrame);
    view->setMouseTracking(true);
    view->viewport()->setAttribute(Qt::WA_Hover);
    return view;
}

QLabel* MakeEmptyLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, DimColor());
    label->setPalette(pal);
    return label;
}

class NextendoToggleSwitch : public QWidget {
public:
    explicit NextendoToggleSwitch(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(46, 26);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    void SetChecked(bool checked_) {
        checked = checked_;
        update();
    }
    bool IsChecked() const {
        return checked;
    }

    std::function<void(bool)> on_toggled;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            Toggle();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter) {
            Toggle();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF track = rect().adjusted(1, 1, -1, -1);
        const QColor track_color = checked ? AccentColor() : QColor(255, 255, 255, 35);
        QPainterPath track_path;
        track_path.addRoundedRect(track, track.height() / 2.0, track.height() / 2.0);
        painter.fillPath(track_path, track_color);

        if (hasFocus()) {
            painter.setPen(QPen(AccentColor(), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(track_path);
        }

        const qreal knob_d = track.height() - 6;
        const qreal knob_x = checked ? track.right() - knob_d - 3 : track.left() + 3;
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QRectF(knob_x, track.top() + 3, knob_d, knob_d));
    }

private:
    void Toggle() {
        checked = !checked;
        update();
        if (on_toggled) {
            on_toggled(checked);
        }
    }

    bool checked = false;
};

class NextendoDropdownRow : public ClickableFrame {
public:
    NextendoDropdownRow(const QString& text, bool selected, QWidget* parent = nullptr)
        : ClickableFrame(parent) {
        setObjectName(QStringLiteral("dropdownRow"));
        setStyleSheet(
            QStringLiteral("QFrame#dropdownRow { background: transparent; border-radius: 8px; }"
                          "QFrame#dropdownRow:hover { background: rgba(255,255,255,20); }"));
        auto* label = new QLabel(selected ? QStringLiteral("✓  ") + text : QStringLiteral("     ") + text);
        if (selected) {
            QFont f = label->font();
            f.setBold(true);
            label->setFont(f);
            label->setStyleSheet(QStringLiteral("color: %1;").arg(AccentColor().name()));
        } else {
            label->setStyleSheet(QStringLiteral("color: rgba(255,255,255,220);"));
        }
        auto* row_layout = new QHBoxLayout(this);
        row_layout->setContentsMargins(10, 8, 10, 8);
        row_layout->addWidget(label);
    }
};

class NextendoDropdown : public QWidget {
public:
    explicit NextendoDropdown(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(34);
        setMinimumWidth(160);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    void SetValues(QStringList values_, int current) {
        values = std::move(values_);
        index = std::clamp(current, 0, static_cast<int>(values.size()) - 1);
        update();
    }
    int CurrentIndex() const {
        return index;
    }

    std::function<void(int)> on_changed;
    std::function<void(QWidget*)> on_popup_opened;
    std::function<void()> on_popup_closed;

    void OpenPopup() {
        if (values.isEmpty() || popup) {
            return;
        }
        QWidget* root = window();
        popup = new QFrame(root);
        popup->setObjectName(QStringLiteral("dropdownPopup"));
        popup->setStyleSheet(QStringLiteral(
            "QFrame#dropdownPopup { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
            "  stop:0 rgba(32, 32, 38, 250), stop:1 rgba(22, 22, 27, 255)); "
            "  border: 1px solid rgba(255,255,255,40); border-radius: 10px; }"));

        auto* layout = new QVBoxLayout(popup);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(2);
        NextendoDropdownRow* first_row = nullptr;
        for (int i = 0; i < values.size(); ++i) {
            auto* row = new NextendoDropdownRow(values[i], i == index, popup);
            row->on_click = [this, i] { SetIndex(i); ClosePopup(); };
            layout->addWidget(row);
            if (!first_row) {
                first_row = row;
            }
        }

        popup->adjustSize();
        popup->setFixedWidth(std::max(width(), popup->sizeHint().width()));
        QPoint local_pos = mapTo(root, QPoint(0, height() + 4));
        if (local_pos.y() + popup->height() > root->height()) {
            local_pos = mapTo(root, QPoint(0, -popup->height() - 4));
        }
        if (local_pos.x() + popup->width() > root->width()) {
            local_pos.setX(root->width() - popup->width());
        }
        popup->move(local_pos);
        popup->show();
        popup->raise();
        if (first_row) {
            first_row->setFocus();
        }
        if (on_popup_opened) {
            on_popup_opened(popup);
        }
    }

    void ClosePopup() {
        if (!popup) {
            return;
        }
        popup->deleteLater();
        popup = nullptr;
        setFocus();
        if (on_popup_closed) {
            on_popup_closed();
        }
    }

    bool IsPopupOpen() const {
        return popup != nullptr;
    }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            popup ? ClosePopup() : OpenPopup();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter) {
            OpenPopup();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF box = rect().adjusted(1, 1, -1, -1);
        QPainterPath box_path;
        box_path.addRoundedRect(box, 8, 8);
        painter.fillPath(box_path, QColor(255, 255, 255, 20));
        painter.setPen(QPen(hasFocus() ? AccentColor() : QColor(255, 255, 255, 30), hasFocus() ? 2 : 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(box_path);

        painter.setPen(QColor(255, 255, 255, 220));
        painter.setFont(font());
        const QString text = values.isEmpty() ? QString{} : values[index];
        painter.drawText(box.adjusted(12, 0, -24, 0), Qt::AlignVCenter | Qt::AlignLeft, text);

        QFont chevron_font = font();
        chevron_font.setBold(true);
        painter.setFont(chevron_font);
        painter.setPen(hasFocus() ? AccentColor() : DimColor());
        painter.drawText(QRectF(box.right() - 22, box.top(), 22, box.height()), Qt::AlignCenter,
                         QStringLiteral("▾"));
    }

private:
    void SetIndex(int new_index) {
        if (values.isEmpty()) {
            return;
        }
        index = new_index;
        update();
        if (on_changed) {
            on_changed(index);
        }
    }

    QStringList values;
    int index = 0;
    QPointer<QFrame> popup;
};

class NextendoSignalIcon : public QWidget {
public:
    explicit NextendoSignalIcon(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(26, 16);
        auto* timer = new QTimer(this);
        timer->setInterval(45);
        connect(timer, &QTimer::timeout, this, [this] {
            if (!online) {
                return;
            }
            phase += 0.09;
            update();
        });
        timer->start();
    }

    void SetOnline(bool online_) {
        online = online_;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        constexpr int kBars = 3;
        constexpr qreal kBarWidth = 5.0;
        constexpr qreal kGap = 3.0;

        for (int i = 0; i < kBars; ++i) {
            const qreal bar_h = height() * (0.42 + 0.29 * i);
            const qreal x = i * (kBarWidth + kGap);
            const qreal y = height() - bar_h;
            QPainterPath path;
            path.addRoundedRect(QRectF(x, y, kBarWidth, bar_h), 1.5, 1.5);

            QColor color;
            if (online) {
                const qreal wave = 0.5 + 0.5 * std::sin(phase - i * 1.1);
                color = QColor(50, 195, 85);
                color.setAlphaF(0.45 + 0.55 * wave);
            } else {
                color = QColor(224, 57, 62, 210);
            }
            painter.fillPath(path, color);
        }

        if (!online) {
            painter.setPen(QPen(QColor(224, 57, 62), 2.2, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(0, 0), QPointF(width(), height()));
            painter.drawLine(QPointF(0, height()), QPointF(width(), 0));
        }
    }

private:
    bool online = true;
    qreal phase = 0.0;
};

} // Anonymous namespace

class NextendoPageNavArrow : public QWidget {
public:
    enum class Side { Left, Right };

    explicit NextendoPageNavArrow(Side side_, QWidget* parent = nullptr)
        : QWidget(parent), side(side_) {
        setFixedHeight(40);
        setMinimumWidth(190); // fits "Recently Played", the longest page title
    }

    void SetState(bool enabled_, const QString& label_) {
        widget_enabled = enabled_;
        label = label_;
        setCursor(widget_enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    std::function<void()> on_click;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (widget_enabled && event->button() == Qt::LeftButton && on_click) {
            on_click();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QColor fg = widget_enabled ? AccentColor() : DimColor();
        const qreal alpha = widget_enabled ? 1.0 : 0.35;

        QColor bubble = fg;
        bubble.setAlphaF(0.16 * alpha);
        QPainterPath bubble_path;
        const int bubble_size = 26;
        const QRect bubble_rect =
            side == Side::Left ? QRect(4, (height() - bubble_size) / 2, bubble_size, bubble_size)
                               : QRect(width() - bubble_size - 4, (height() - bubble_size) / 2,
                                       bubble_size, bubble_size);
        bubble_path.addEllipse(bubble_rect);
        painter.fillPath(bubble_path, bubble);
        painter.setPen(QPen(fg, 1.4));
        painter.drawPath(bubble_path);

        QFont bubble_font = font();
        bubble_font.setBold(true);
        painter.setFont(bubble_font);
        painter.setPen(fg);
        painter.drawText(bubble_rect, Qt::AlignCenter, side == Side::Left ? tr("L") : tr("R"));

        QFont label_font = font();
        label_font.setPointSize(label_font.pointSize() - 1);
        painter.setFont(label_font);
        QColor label_color = fg;
        label_color.setAlphaF(alpha);
        painter.setPen(label_color);

        const int text_margin = bubble_size + 10;
        const QRect text_rect = side == Side::Left
                                    ? QRect(text_margin, 0, width() - text_margin - 4, height())
                                    : QRect(4, 0, width() - text_margin - 4, height());
        painter.drawText(text_rect,
                         (side == Side::Left ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                         QFontMetrics(label_font).elidedText(label, Qt::ElideRight, text_rect.width()));
    }

private:
    Side side;
    bool widget_enabled = false;
    QString label;
};


NextendoAccountDialog::NextendoAccountDialog(NextendoController* controller_,
                                             Core::System& system_, QWidget* parent,
                                             int initial_page_)
    : QDialog(parent), controller(controller_), system(system_), hid_core(system_.HIDCore()),
      initial_page(initial_page_) {
    setWindowTitle(tr("Nextendo Account"));
    setFixedSize(999, 598);

    // Otherwise the same physical controller drives both this dialog's navigation and a
    // running game simultaneously -- restored in the destructor.
    hid_core.SetGuestInputSuspended(true);

    controller_navigation = new ControllerNavigation(hid_core, this);
    WireControllerNav();

    header_card = new HeaderCard(this);
    header_avatar = new QLabel;
    header_avatar->setFixedSize(kHeaderAvatarSize, kHeaderAvatarSize);
    header_avatar->setPixmap(RoundedPixmap({}, kHeaderAvatarSize));
    header_avatar->setCursor(Qt::PointingHandCursor);
    header_avatar->setToolTip(tr("Click to change your profile picture"));
    header_avatar->installEventFilter(this);

    header_name = new QLabel;
    QFont name_font = header_name->font();
    name_font.setPointSize(name_font.pointSize() + 4);
    name_font.setBold(true);
    header_name->setFont(name_font);
    header_name->setStyleSheet(QStringLiteral("QLabel { color: white; }"));
    auto* name_shadow = new QGraphicsDropShadowEffect(header_name);
    name_shadow->setBlurRadius(12);
    name_shadow->setColor(QColor(0, 0, 0, 230));
    name_shadow->setOffset(0, 1);
    header_name->setGraphicsEffect(name_shadow);

    edit_name_button = new QToolButton;
    edit_name_button->setText(QStringLiteral("✎"));
    edit_name_button->setCursor(Qt::PointingHandCursor);
    edit_name_button->setToolTip(tr("Change your username"));
    edit_name_button->setAutoRaise(true);
    edit_name_button->setFocusPolicy(Qt::NoFocus);
    connect(edit_name_button, &QToolButton::clicked, this,
           &NextendoAccountDialog::OnEditUsername);

    auto* header_name_row = new QHBoxLayout;
    header_name_row->setSpacing(4);
    header_name_row->addWidget(header_name);
    header_name_row->addWidget(edit_name_button);
    header_name_row->addStretch();

    header_code = new QLabel;
    header_code->setCursor(Qt::PointingHandCursor);
    header_code->setToolTip(tr("Click to copy"));
    header_code->installEventFilter(this);
    QFont code_font(QStringLiteral("monospace"));
    header_code->setFont(code_font);
    header_code->setStyleSheet(QStringLiteral("QLabel { padding: 2px 8px; border-radius: 8px; "
                                              "background: rgba(0,0,0,150); color: white; }"));

    auto* header_text = new QVBoxLayout;
    header_text->setSpacing(6);
    header_text->addStretch();
    header_text->addLayout(header_name_row);
    header_text->addWidget(header_code, 0, Qt::AlignLeft);
    header_text->addStretch();

    auto* header_content = new QHBoxLayout;
    header_content->setSpacing(16);
    header_content->addWidget(header_avatar);
    header_content->addLayout(header_text);

    auto* background_button = new QToolButton;
    background_button->setText(QStringLiteral("🖼"));
    background_button->setCursor(Qt::PointingHandCursor);
    background_button->setToolTip(tr("Change the background image"));
    background_button->setAutoRaise(true);
    background_button->setFocusPolicy(Qt::NoFocus);
    background_button->setPopupMode(QToolButton::InstantPopup);
    background_button->setStyleSheet(QStringLiteral("QToolButton { color: white; }"));
    auto* background_menu = new QMenu(background_button);
    background_menu->addAction(tr("Choose Background Image..."), this,
                               &NextendoAccountDialog::OnChangeBackground);
    background_menu->addAction(tr("Remove Background"), this,
                               &NextendoAccountDialog::OnRemoveBackground);
    background_button->setMenu(background_menu);

    auto* header_top_row = new QHBoxLayout;
    header_top_row->addStretch();
    header_top_row->addWidget(background_button);

    auto* header_middle_row = new QHBoxLayout;
    header_middle_row->addStretch();
    header_middle_row->addLayout(header_content);
    header_middle_row->addStretch();

    auto* header_outer = new QVBoxLayout(header_card);
    header_outer->setContentsMargins(10, 6, 10, 14);
    header_outer->addLayout(header_top_row);
    header_outer->addLayout(header_middle_row);
    header_outer->addStretch();
    header_card->setMinimumHeight(200);
    header_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* status_card = MakeDashCard();
    auto* status_card_layout = new QVBoxLayout(status_card);
    status_card_layout->setContentsMargins(18, 16, 18, 16);
    status_card_layout->setSpacing(10);
    auto* status_title = MakeCardTitle(tr("Nextendo Status"));
    status_title->setAlignment(Qt::AlignCenter);
    status_card_layout->addWidget(status_title);

    auto* online_icon = new NextendoSignalIcon;
    dash_online_dot = online_icon;
    dash_online_text = new QLabel;
    dash_online_text->setAlignment(Qt::AlignCenter);
    const bool nextendo_enabled = Settings::values.enable_nextendo.GetValue();
    online_icon->SetOnline(nextendo_enabled);
    if (nextendo_enabled) {
        dash_online_text->setText(tr("Online"));
        dash_online_text->setStyleSheet(QStringLiteral("color: #3fdb76; font-weight: 600;"));
    } else {
        dash_online_text->setText(tr("Offline: Please enable \"Nextendo Redirection\""));
        dash_online_text->setStyleSheet(QStringLiteral("color: #ff5b56; font-weight: 600;"));
        dash_online_text->setWordWrap(true);
    }

    auto* online_row = new QHBoxLayout;
    online_row->setSpacing(10);
    online_row->addStretch();
    online_row->addWidget(dash_online_dot, 0, Qt::AlignVCenter);
    online_row->addWidget(dash_online_text, 0, Qt::AlignVCenter);
    online_row->addStretch();
    status_card_layout->addLayout(online_row);

    auto* stats_divider = new QFrame;
    stats_divider->setFrameShape(QFrame::HLine);
    stats_divider->setStyleSheet(QStringLiteral("color: rgba(255,255,255,20);"));
    status_card_layout->addWidget(stats_divider);

    auto* stats_row = new QHBoxLayout;
    stats_row->setSpacing(0);
    const auto make_stat = [&](const QString& label, QLabel*& value_out) {
        auto* col = new QVBoxLayout;
        col->setSpacing(4);
        auto* label_widget = MakeDimLabel(label);
        label_widget->setAlignment(Qt::AlignHCenter);
        col->addWidget(label_widget);
        value_out = new QLabel(QStringLiteral("0"));
        QFont vf = value_out->font();
        vf.setBold(true);
        vf.setPointSize(vf.pointSize() + 7);
        value_out->setFont(vf);
        value_out->setAlignment(Qt::AlignHCenter);
        col->addWidget(value_out);
        stats_row->addLayout(col, 1);
    };
    make_stat(tr("Friends Online"), dash_friends_online_value);

    auto* stats_vline = new QFrame;
    stats_vline->setFrameShape(QFrame::VLine);
    stats_vline->setStyleSheet(QStringLiteral("color: rgba(255,255,255,20);"));
    stats_row->addWidget(stats_vline);

    make_stat(tr("In-Game"), dash_in_game_value);
    status_card_layout->addLayout(stats_row);

    dash_requests_card = new ClickableFrame;
    dash_requests_card->setStyleSheet(DashCardStyle());
    dash_requests_card->setFocusPolicy(Qt::NoFocus);
    static_cast<ClickableFrame*>(dash_requests_card)->on_click = [this] {
        // [Nextendo] Requests live on the Friends page now.
        GoToPage(page_titles.indexOf(tr("Friends")));
    };
    auto* requests_card_layout = new QVBoxLayout(dash_requests_card);
    requests_card_layout->setContentsMargins(18, 16, 18, 16);
    requests_card_layout->setSpacing(8);

    dash_requests_badge = new QLabel;
    dash_requests_badge->setFixedSize(20, 20);
    dash_requests_badge->setAlignment(Qt::AlignCenter);
    dash_requests_badge->setStyleSheet(
        QStringLiteral("background: #e0393e; color: white; font-size: 11px; font-weight: bold; "
                       "border-radius: 10px;"));
    dash_requests_badge->hide();

    auto* requests_header_row = new QHBoxLayout;
    requests_header_row->addWidget(MakeCardTitle(tr("Friend Requests")));
    requests_header_row->addWidget(dash_requests_badge);
    requests_header_row->addStretch();
    requests_header_row->addWidget(MakeDimLabel(QStringLiteral("›")));
    requests_card_layout->addLayout(requests_header_row);

    dash_requests_preview = MakeDimLabel(tr("No pending requests"));
    dash_requests_preview->setWordWrap(true);
    requests_card_layout->addWidget(dash_requests_preview);
    requests_card_layout->addStretch(1);

    auto* friends_card = MakeDashCard();
    auto* friends_card_layout = new QVBoxLayout(friends_card);
    friends_card_layout->setContentsMargins(18, 16, 18, 16);
    friends_card_layout->setSpacing(8);

    auto* view_all = new ClickableFrame;
    view_all->setFrameShape(QFrame::NoFrame);
    view_all->setFocusPolicy(Qt::NoFocus);
    view_all->on_click = [this] { GoToPage(page_titles.indexOf(tr("Friends"))); };
    auto* view_all_label = new QLabel(tr("View All ›"));
    view_all_label->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 600;").arg(AccentColor().name()));
    auto* view_all_layout = new QHBoxLayout(view_all);
    view_all_layout->setContentsMargins(0, 0, 0, 0);
    view_all_layout->addWidget(view_all_label);

    auto* friends_header_row = new QHBoxLayout;
    friends_header_row->addWidget(MakeCardTitle(tr("Friends List")));
    friends_header_row->addStretch();
    friends_header_row->addWidget(view_all);
    friends_card_layout->addLayout(friends_header_row);

    auto* friends_preview_container = new QWidget;
    dash_friends_list_layout = new QVBoxLayout(friends_preview_container);
    dash_friends_list_layout->setContentsMargins(0, 0, 4, 0);
    dash_friends_list_layout->setSpacing(10);
    dash_friends_list_layout->addStretch(1); // pushes rows to the top once they run out

    auto* friends_preview_scroll = new QScrollArea;
    friends_preview_scroll->setWidget(friends_preview_container);
    friends_preview_scroll->setWidgetResizable(true);
    friends_preview_scroll->setFrameShape(QFrame::NoFrame);
    friends_preview_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    friends_preview_scroll->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
    friends_preview_scroll->setFocusPolicy(Qt::NoFocus);
    friends_preview_scroll->verticalScrollBar()->setFocusPolicy(Qt::NoFocus);
    friends_card_layout->addWidget(friends_preview_scroll, 1);

    // Narrow sidebar card: settings stack per-row instead of one wide strip.
    auto* home_settings_card = MakeDashCard();
    auto* home_settings_layout = new QVBoxLayout(home_settings_card);
    home_settings_layout->setContentsMargins(18, 16, 18, 16);
    home_settings_layout->setSpacing(2);

    const auto add_settings_divider = [&] {
        auto* divider = new QFrame;
        divider->setFrameShape(QFrame::HLine);
        divider->setStyleSheet(QStringLiteral("color: rgba(255,255,255,20);"));
        home_settings_layout->addWidget(divider);
    };
    const auto make_row_icon = [](const QString& glyph) {
        auto* label = new QLabel(glyph);
        QFont f = label->font();
        f.setPointSize(f.pointSize() + 4);
        label->setFont(f);
        label->setFixedWidth(24);
        label->setAlignment(Qt::AlignCenter);
        return label;
    };
    const auto make_row_title = [](const QString& text) {
        auto* label = new QLabel(text);
        QFont f = label->font();
        f.setBold(true);
        label->setFont(f);
        return label;
    };
    const auto make_value_pill = [](const QString& text, int fixed_width) {
        auto* label = new QLabel(text);
        label->setAlignment(Qt::AlignCenter);
        label->setFixedWidth(fixed_width);
        label->setStyleSheet(QStringLiteral(
            "background: rgba(255,255,255,25); color: %1; font-weight: 600; "
            "border-radius: 9px; padding: 2px 10px;")
            .arg(DimColor().name()));
        return label;
    };
    const auto make_settings_row = [&](QLayout* row) {
        auto* row_widget = new QWidget;
        row_widget->setLayout(row);
        home_settings_layout->addWidget(row_widget);
    };

    auto* nat_row = new QHBoxLayout;
    nat_row->setSpacing(8);
    nat_row->addWidget(make_row_icon(QStringLiteral("📶")));
    nat_row->addWidget(make_row_title(tr("NAT Type")));
    nat_row->addStretch(1);
    nat_label = make_value_pill(tr("Not Tested"), 96);
    nat_row->addWidget(nat_label);
    make_settings_row(nat_row);

    auto* ping_row = new QHBoxLayout;
    ping_row->setSpacing(8);
    ping_row->addWidget(make_row_icon(QStringLiteral("⏱")));
    ping_row->addWidget(make_row_title(tr("Ping")));
    ping_row->addStretch(1);
    ping_label = make_value_pill(QStringLiteral("--"), 96);
    ping_row->addWidget(ping_label);
    make_settings_row(ping_row);
    home_settings_layout->addSpacing(6);

    auto* test_connection_button = new QPushButton(tr("Test Connection"));
    test_connection_button->setCursor(Qt::PointingHandCursor);
    test_connection_button->setMinimumHeight(30);
    test_connection_button->setStyleSheet(
        QStringLiteral("QPushButton { background: rgba(255,255,255,25); color: rgba(255,255,255,220); "
                       "border: 2px solid transparent; border-radius: 8px; padding: 4px 16px; "
                       "font-weight: 600; }"
                       "QPushButton:hover { background: rgba(255,255,255,40); }"
                       "QPushButton:focus { border: 2px solid %1; }"
                       "QPushButton:disabled { background: rgba(128,128,128,40); "
                       "color: rgba(255,255,255,90); }")
            .arg(AccentColor().name()));
    home_settings_layout->addWidget(test_connection_button);
    home_default_focus = test_connection_button;
    home_settings_layout->addSpacing(6);
    add_settings_divider();

    auto* notifications_row = new QHBoxLayout;
    notifications_row->setSpacing(8);
    notifications_row->addWidget(make_row_icon(QStringLiteral("🔔")));
    notifications_row->addWidget(make_row_title(tr("Notifications")));
    notifications_row->addStretch(1);
    auto* notifications_toggle = new NextendoToggleSwitch;
    notifications_toggle->SetChecked(UISettings::values.nextendo_notifications_enabled.GetValue());
    notifications_toggle->on_toggled = [](bool checked) {
        UISettings::values.nextendo_notifications_enabled.SetValue(checked);
    };
    notifications_row->addWidget(notifications_toggle);
    make_settings_row(notifications_row);
    add_settings_divider();

    auto* corner_label_row = new QHBoxLayout;
    corner_label_row->setSpacing(8);
    corner_label_row->addWidget(make_row_icon(QStringLiteral("📍")));
    corner_label_row->addWidget(make_row_title(tr("Notification Corner")));
    corner_label_row->addStretch(1);
    make_settings_row(corner_label_row);

    auto* notification_corner = new NextendoDropdown;
    notification_corner->SetValues({tr("Top Right"), tr("Top Left"), tr("Bottom Right"),
                                    tr("Bottom Left")},
                                   std::clamp(UISettings::values.nextendo_notification_corner.GetValue(), 0, 3));
    notification_corner->on_popup_opened = [this](QWidget* popup) { active_popup = popup; };
    notification_corner->on_popup_closed = [this] { active_popup = nullptr; };
    notification_corner->on_changed = [](int index) {
        UISettings::values.nextendo_notification_corner.SetValue(index);
    };
    home_settings_layout->addSpacing(4);
    home_settings_layout->addWidget(notification_corner);
    home_settings_layout->addStretch(1);

    auto* right_column = new QVBoxLayout;
    right_column->setSpacing(12);
    right_column->addWidget(status_card);
    right_column->addWidget(home_settings_card, 1);

    auto* right_column_widget = new QWidget;
    right_column_widget->setLayout(right_column);
    right_column_widget->setFixedWidth(300);

    // Friends List (65%) and Friend Requests (35%) beneath the banner.
    auto* below_header_row = new QHBoxLayout;
    below_header_row->setSpacing(12);
    below_header_row->addWidget(friends_card, 65);
    below_header_row->addWidget(dash_requests_card, 35);

    auto* left_column = new QVBoxLayout;
    left_column->setSpacing(12);
    left_column->addWidget(header_card);
    left_column->addLayout(below_header_row, 1);

    auto* home_layout = new QHBoxLayout;
    home_layout->setSpacing(16);
    home_layout->addLayout(left_column, 1);
    home_layout->addWidget(right_column_widget);

    auto* home_page = new QWidget;
    auto* home_page_outer = new QVBoxLayout(home_page);
    home_page_outer->setContentsMargins(0, 0, 0, 0);
    home_page_outer->addLayout(home_layout);

    const QString field_style = QStringLiteral(
        "QLineEdit { background: rgba(255,255,255,15); border: 1px solid rgba(255,255,255,25); "
        "border-radius: 8px; padding: 9px 14px; color: white; }"
        "QLineEdit:focus { border: 1px solid %1; }")
            .arg(AccentColor().name());

    friend_code_input = new QLineEdit;
    friend_code_input->setPlaceholderText(tr("SW-0000-0000-0000"));
    friend_code_input->setStyleSheet(field_style);
    add_button = new QPushButton(tr("Add Friend"));
    add_button->setCursor(Qt::PointingHandCursor);
    add_button->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: white; border: none; "
                       "border-radius: 8px; padding: 9px 20px; font-weight: 600; }"
                       "QPushButton:disabled { background: rgba(128,128,128,60); "
                       "color: rgba(255,255,255,110); }")
            .arg(AccentColor().name()));
    auto* add_row = new QHBoxLayout;
    add_row->setSpacing(10);
    add_row->addWidget(friend_code_input, 1);
    add_row->addWidget(add_button);

    friend_search = new QLineEdit;
    friend_search->setPlaceholderText(tr("Search friends..."));
    friend_search->setClearButtonEnabled(true);
    friend_search->setStyleSheet(field_style);

    friends_view = MakeCardList(this);
    friends_model = new QStandardItemModel(this);
    friends_view->setModel(friends_model);
    friend_delegate = new NextendoFriendDelegate(friends_view, this);
    friends_view->setItemDelegate(friend_delegate);
    connect(friends_view, &QListView::clicked, this, &NextendoAccountDialog::OnFriendsViewClicked);
    friends_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(friends_view, &QListView::customContextMenuRequested, this,
           &NextendoAccountDialog::ShowFriendsContextMenu);
    connect(friend_search, &QLineEdit::textChanged, this, &NextendoAccountDialog::ApplyFriendFilter);

    friends_stack = new QStackedWidget;
    friends_stack->addWidget(friends_view);
    friends_stack->addWidget(MakeEmptyLabel(tr("No friends yet — add one by friend code above.")));

    // [Nextendo] Friends and Requests, side-by-side on one page.
    auto* friends_column_card = MakeDashCard();
    auto* friends_column_layout = new QVBoxLayout(friends_column_card);
    friends_column_layout->setContentsMargins(18, 16, 18, 16);
    friends_column_layout->setSpacing(10);
    friends_column_layout->addWidget(MakeCardTitle(tr("Friends")));
    friends_column_layout->addLayout(add_row);
    friends_column_layout->addWidget(friend_search);
    friends_column_layout->addWidget(friends_stack, 1);

    requests_view = MakeCardList(this);
    requests_model = new QStandardItemModel(this);
    requests_view->setModel(requests_model);
    request_delegate = new NextendoFriendDelegate(requests_view, this);
    requests_view->setItemDelegate(request_delegate);
    connect(requests_view, &QListView::clicked, this, &NextendoAccountDialog::OnFriendsViewClicked);
    requests_stack = new QStackedWidget;
    requests_stack->addWidget(requests_view);
    requests_stack->addWidget(MakeEmptyLabel(tr("No incoming friend requests.")));

    outgoing_requests_label = new QLabel(tr("Outgoing Friend Requests"));
    QFont outgoing_label_font = outgoing_requests_label->font();
    outgoing_label_font.setBold(true);
    outgoing_requests_label->setFont(outgoing_label_font);

    outgoing_requests_view = MakeCardList(this);
    outgoing_requests_model = new QStandardItemModel(this);
    outgoing_requests_view->setModel(outgoing_requests_model);
    outgoing_request_delegate = new NextendoFriendDelegate(outgoing_requests_view, this);
    outgoing_requests_view->setItemDelegate(outgoing_request_delegate);
    connect(outgoing_requests_view, &QListView::clicked, this,
            &NextendoAccountDialog::OnFriendsViewClicked);

    outgoing_requests_section = new QWidget;
    auto* outgoing_section_layout = new QVBoxLayout(outgoing_requests_section);
    outgoing_section_layout->setContentsMargins(0, 8, 0, 0);
    outgoing_section_layout->setSpacing(6);
    outgoing_section_layout->addWidget(outgoing_requests_label);
    outgoing_section_layout->addWidget(outgoing_requests_view);
    outgoing_requests_section->setVisible(false);

    auto* requests_column_card = MakeDashCard();
    auto* requests_column_layout = new QVBoxLayout(requests_column_card);
    requests_column_layout->setContentsMargins(18, 16, 18, 16);
    requests_column_layout->setSpacing(10);
    requests_column_layout->addWidget(MakeCardTitle(tr("Requests")));
    requests_column_layout->addWidget(requests_stack, 1);
    requests_column_layout->addWidget(outgoing_requests_section);

    auto* friends_requests_row = new QHBoxLayout;
    friends_requests_row->setSpacing(16);
    friends_requests_row->addWidget(friends_column_card, 1);
    friends_requests_row->addWidget(requests_column_card, 1);

    auto* friends_page = new QWidget;
    auto* friends_page_layout = new QVBoxLayout(friends_page);
    friends_page_layout->setContentsMargins(0, 0, 0, 0);
    friends_page_layout->addLayout(friends_requests_row);

    history_view = MakeCardList(this);
    history_model = new QStandardItemModel(this);
    history_view->setModel(history_model);
    history_view->setItemDelegate(new NextendoHistoryDelegate(history_view, this));
    connect(history_view, &QListView::clicked, this, &NextendoAccountDialog::OnHistoryViewClicked);
    history_stack = new QStackedWidget;
    history_stack->addWidget(history_view);
    history_stack->addWidget(MakeEmptyLabel(tr("No games played yet.")));

    cloud_save_icon = new QLabel;
    cloud_save_icon->setFixedSize(64, 64);
    cloud_save_icon->setAlignment(Qt::AlignCenter);

    cloud_save_title = new QLabel;
    QFont cloud_save_title_font = cloud_save_title->font();
    cloud_save_title_font.setBold(true);
    cloud_save_title_font.setPointSize(cloud_save_title_font.pointSize() + 1);
    cloud_save_title->setFont(cloud_save_title_font);
    cloud_save_title->setAlignment(Qt::AlignCenter);

    cloud_save_picker_group = new QButtonGroup(this);
    cloud_save_picker_row = new QHBoxLayout;
    cloud_save_picker_row->setSpacing(10);
    cloud_save_picker_container = new QWidget;
    cloud_save_picker_container->setLayout(cloud_save_picker_row);

    cloud_save_status = new QLabel;
    cloud_save_status->setWordWrap(true);
    cloud_save_status->setAlignment(Qt::AlignCenter);
    QPalette cloud_save_status_pal = cloud_save_status->palette();
    cloud_save_status_pal.setColor(QPalette::WindowText, DimColor());
    cloud_save_status->setPalette(cloud_save_status_pal);

    cloud_save_download_button = new QPushButton(tr("\xE2\x86\x93 Download Save"));
    cloud_save_download_button->setCursor(Qt::PointingHandCursor);
    cloud_save_download_button->setMinimumHeight(34);
    cloud_save_download_button->setMinimumWidth(180);
    cloud_save_download_button->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: white; border: none; "
                       "border-radius: 8px; padding: 6px 18px; font-weight: 600; }"
                       "QPushButton:disabled { background: rgba(128,128,128,60); "
                       "color: rgba(255,255,255,110); }")
            .arg(AccentColor().name()));
    connect(cloud_save_download_button, &QPushButton::clicked, this, [this] {
        if (cloud_save_selected_title_id != 0) {
            controller->ManualSaveDownload(cloud_save_selected_title_id);
        }
    });

    auto* cloud_save_buttons = new QHBoxLayout;
    cloud_save_buttons->addStretch(1);
    cloud_save_buttons->addWidget(cloud_save_download_button);
    cloud_save_buttons->addStretch(1);

    // Governs only the automatic pull-on-boot/push-on-stop sync -- not the manual Download
    // Save button above, which is already an explicit action each time it's clicked.
    cloud_save_auto_sync_checkbox = new QCheckBox(tr("Automatically sync cloud saves"));
    cloud_save_auto_sync_checkbox->setChecked(Settings::values.nextendo_cloud_sync_enabled.GetValue());
    cloud_save_auto_sync_checkbox->setCursor(Qt::PointingHandCursor);
    connect(cloud_save_auto_sync_checkbox, &QCheckBox::toggled, this,
            [](bool checked) { Settings::values.nextendo_cloud_sync_enabled.SetValue(checked); });

    auto* cloud_save_card = new QFrame;
    cloud_save_card->setStyleSheet(DashCardStyle());
    auto* cloud_save_card_layout = new QVBoxLayout(cloud_save_card);
    cloud_save_card_layout->setContentsMargins(28, 28, 28, 28);
    cloud_save_card_layout->setSpacing(6);
    cloud_save_card_layout->addWidget(cloud_save_icon, 0, Qt::AlignHCenter);
    cloud_save_card_layout->addWidget(cloud_save_title, 0, Qt::AlignHCenter);
    cloud_save_card_layout->addWidget(cloud_save_picker_container, 0, Qt::AlignHCenter);
    cloud_save_card_layout->addSpacing(6);
    cloud_save_card_layout->addWidget(cloud_save_status);
    cloud_save_card_layout->addSpacing(12);
    cloud_save_card_layout->addLayout(cloud_save_buttons);
    cloud_save_card_layout->addSpacing(12);
    cloud_save_card_layout->addWidget(cloud_save_auto_sync_checkbox, 0, Qt::AlignHCenter);

    auto* cloud_save_page = new QWidget;
    auto* cloud_save_layout = new QVBoxLayout(cloud_save_page);
    cloud_save_layout->setContentsMargins(20, 20, 20, 20);
    cloud_save_layout->addStretch(1);
    cloud_save_layout->addWidget(cloud_save_card);
    cloud_save_layout->addStretch(2);

    lobby_state_label = MakeDimLabel(tr("Not in a lobby."));
    lobby_state_label->setAlignment(Qt::AlignCenter);

    lobby_view = MakeCardList(this);
    lobby_model = new QStandardItemModel(this);
    lobby_view->setModel(lobby_model);
    lobby_delegate = new NextendoFriendDelegate(lobby_view, this);
    lobby_view->setItemDelegate(lobby_delegate);
    lobby_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(lobby_view, &QListView::customContextMenuRequested, this,
           [this](const QPoint& pos) { ShowPlayersContextMenu(lobby_view, pos); });
    connect(lobby_view, &QListView::clicked, this, &NextendoAccountDialog::OnPlayersViewClicked);

    lobby_stack = new QStackedWidget;
    lobby_stack->addWidget(lobby_view);
    lobby_stack->addWidget(MakeEmptyLabel(tr("Not in a lobby.")));

    auto* lobby_card = MakeDashCard();
    auto* lobby_card_layout = new QVBoxLayout(lobby_card);
    lobby_card_layout->setContentsMargins(18, 16, 18, 16);
    lobby_card_layout->setSpacing(8);
    lobby_card_layout->addWidget(MakeCardTitle(tr("Current Lobby")));
    lobby_card_layout->addWidget(lobby_state_label);
    lobby_card_layout->addWidget(lobby_stack, 1);

    recent_players_view = MakeCardList(this);
    recent_players_model = new QStandardItemModel(this);
    recent_players_view->setModel(recent_players_model);
    recent_players_delegate = new NextendoFriendDelegate(recent_players_view, this);
    recent_players_view->setItemDelegate(recent_players_delegate);
    recent_players_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(recent_players_view, &QListView::customContextMenuRequested, this,
           [this](const QPoint& pos) { ShowPlayersContextMenu(recent_players_view, pos); });
    connect(recent_players_view, &QListView::clicked, this,
           &NextendoAccountDialog::OnPlayersViewClicked);

    recent_players_stack = new QStackedWidget;
    recent_players_stack->addWidget(recent_players_view);
    recent_players_stack->addWidget(MakeEmptyLabel(tr("No one else seen online recently.")));

    auto* recent_players_card = MakeDashCard();
    auto* recent_players_card_layout = new QVBoxLayout(recent_players_card);
    recent_players_card_layout->setContentsMargins(18, 16, 18, 16);
    recent_players_card_layout->setSpacing(8);
    recent_players_card_layout->addWidget(MakeCardTitle(tr("Recently Encountered")));
    recent_players_card_layout->addWidget(recent_players_stack, 1);

    // [Nextendo] Current Lobby and Recently Encountered, side-by-side.
    auto* players_row = new QHBoxLayout;
    players_row->setSpacing(16);
    players_row->addWidget(lobby_card, 1);
    players_row->addWidget(recent_players_card, 1);

    auto* players_page = new QWidget;
    auto* players_page_layout = new QVBoxLayout(players_page);
    players_page_layout->setContentsMargins(0, 0, 0, 0);
    players_page_layout->addLayout(players_row);

    pages_stack = new QStackedWidget;
    pages_stack->addWidget(home_page);
    pages_stack->addWidget(friends_page);
    pages_stack->addWidget(players_page);
    pages_stack->addWidget(history_stack);
    pages_stack->addWidget(cloud_save_page);

    page_titles = {tr("Home"), tr("Friends"), tr("Players"), tr("Recently Played"),
                  tr("Cloud Saves")};

    page_title_label = new QLabel;
    QFont page_title_font = page_title_label->font();
    page_title_font.setBold(true);
    page_title_font.setPointSize(page_title_font.pointSize() + 1);
    page_title_label->setFont(page_title_font);
    page_title_label->setAlignment(Qt::AlignCenter);

    nav_left_arrow = new NextendoPageNavArrow(NextendoPageNavArrow::Side::Left);
    nav_left_arrow->on_click = [this] { GoToPage(current_page - 1); };
    nav_right_arrow = new NextendoPageNavArrow(NextendoPageNavArrow::Side::Right);
    nav_right_arrow->on_click = [this] { GoToPage(current_page + 1); };

    requests_badge = new QLabel;
    requests_badge->setAlignment(Qt::AlignCenter);
    requests_badge->setFixedSize(18, 18);
    requests_badge->setStyleSheet(
        QStringLiteral("QLabel { background-color: #e0393e; color: white; font-size: 10px; "
                       "font-weight: bold; border-radius: 9px; }"));
    requests_badge->hide();

    auto* nav_bar = new QHBoxLayout;
    nav_bar->addWidget(nav_left_arrow);
    nav_bar->addStretch();
    nav_bar->addWidget(page_title_label);
    nav_bar->addWidget(requests_badge);
    nav_bar->addStretch();
    nav_bar->addWidget(nav_right_arrow);

    status = new QLabel;
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("QLabel { background: rgba(255,255,255,15); "
                                         "color: rgba(255,255,255,200); border-radius: 10px; "
                                         "padding: 8px 14px; }"));

    setStyleSheet(
        QStringLiteral("QPushButton:focus, QToolButton:focus, QCheckBox:focus, "
                       "QComboBox:focus, QLineEdit:focus { "
                       "outline: none; border: 2px solid %1; border-radius: 6px; }")
            .arg(AccentColor().name()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);
    layout->addLayout(nav_bar);
    layout->addWidget(pages_stack, 1);
    layout->addWidget(status);

    network_probe = new NextendoNetworkProbe(this);
    status_check_timer = new QTimer(this);
    status_check_timer->setInterval(350);
    const QString checking_style = QStringLiteral(
        "background: rgba(255,255,255,25); color: %1; font-weight: 600; "
        "border-radius: 9px; padding: 2px 10px;")
        .arg(DimColor().name());
    connect(status_check_timer, &QTimer::timeout, this, [this, checking_style] {
        status_check_dots = (status_check_dots + 1) % 4;
        const QString dots = QString(status_check_dots, QLatin1Char('.'));
        if (nat_checking) {
            nat_label->setText(tr("Checking") + dots);
        }
        if (ping_checking) {
            ping_label->setText(tr("Checking") + dots);
        }
        if (!nat_checking && !ping_checking) {
            status_check_timer->stop();
        }
    });
    connect(network_probe, &NextendoNetworkProbe::NatStatusChanged, this,
            [this, checking_style](NextendoNetworkProbe::NatStatus nat_status) {
                switch (nat_status) {
                case NextendoNetworkProbe::NatStatus::Checking:
                    nat_checking = true;
                    nat_label->setText(tr("Checking"));
                    nat_label->setStyleSheet(checking_style);
                    break;
                case NextendoNetworkProbe::NatStatus::Open:
                    nat_checking = false;
                    nat_label->setText(tr("Open"));
                    nat_label->setStyleSheet(QStringLiteral(
                        "background: rgba(50,195,85,40); color: #32c355; font-weight: 600; "
                        "border-radius: 9px; padding: 2px 10px;"));
                    break;
                case NextendoNetworkProbe::NatStatus::Strict:
                    nat_checking = false;
                    nat_label->setText(tr("Strict"));
                    nat_label->setStyleSheet(QStringLiteral(
                        "background: rgba(232,131,62,40); color: #e8833e; font-weight: 600; "
                        "border-radius: 9px; padding: 2px 10px;"));
                    break;
                case NextendoNetworkProbe::NatStatus::Unknown:
                    nat_checking = false;
                    nat_label->setText(tr("Unknown"));
                    nat_label->setStyleSheet(QStringLiteral(
                        "background: rgba(224,57,62,40); color: #e0393e; font-weight: 600; "
                        "border-radius: 9px; padding: 2px 10px;"));
                    break;
                }
            });
    connect(network_probe, &NextendoNetworkProbe::PingResult, this, [this](int ms) {
        ping_checking = false;
        ping_label->setText(ms >= 0 ? tr("%1 ms").arg(ms) : QStringLiteral("--"));
    });
    connect(test_connection_button, &QPushButton::clicked, this,
            [this, test_connection_button, checking_style] {
                nat_checking = true;
                ping_checking = true;
                status_check_dots = 0;
                nat_label->setText(tr("Checking"));
                nat_label->setStyleSheet(checking_style);
                ping_label->setText(tr("Checking"));
                ping_label->setStyleSheet(checking_style);
                status_check_timer->start();
                network_probe->ProbeNat();
                network_probe->PingBackend();
                test_connection_button->setEnabled(false);
                QTimer::singleShot(10000, test_connection_button, [test_connection_button] {
                    test_connection_button->setEnabled(true);
                });
            });

    connect(add_button, &QPushButton::clicked, this, &NextendoAccountDialog::OnAdd);
    connect(friend_code_input, &QLineEdit::returnPressed, this, &NextendoAccountDialog::OnAdd);

    header_name->setText(QString::fromStdString(Common::NextendoAccount::GetUsername()));
    header_code->setText(QString::fromStdString(Common::NextendoAccount::GetFriendCode()));

    LoadSavedBackground();
    GoToPage(initial_page);
    UpdateHeroSizing();
    RefreshFriends();
    RefreshHistory();
    RefreshCloudSaveTab();
    RefreshPlayers();

    connect(controller, &NextendoController::StatusChanged, this,
            [this, cloud_save_page](const QString& message) {
                if (pages_stack->currentWidget() == cloud_save_page) {
                    cloud_save_status->setText(message);
                }
            });

    refresh_timer.setInterval(15000);
    connect(&refresh_timer, &QTimer::timeout, this, &NextendoAccountDialog::RefreshFriends);
    connect(&refresh_timer, &QTimer::timeout, this, &NextendoAccountDialog::RefreshHistory);
    connect(&refresh_timer, &QTimer::timeout, this, &NextendoAccountDialog::RefreshCloudSaveTab);
    connect(&refresh_timer, &QTimer::timeout, this, &NextendoAccountDialog::RefreshPlayers);
    refresh_timer.start();

    connect(pages_stack, &QStackedWidget::currentChanged, this, [this](int index) {
        if (pages_stack->widget(index) == history_stack) {
            RefreshHistory();
        }
    });

#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, guard = QPointer<NextendoAccountDialog>(this)] {
        auto profile = WebService::NextendoApi::GetProfile();
        if (!profile.ok || profile.image_base64.empty() || !guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, image = profile.image_base64] {
                if (!guard) {
                    return;
                }
                const QPixmap avatar = Nextendo::AvatarCache::Get("self", image, 256);
                if (!avatar.isNull()) {
                    header_avatar_source = avatar;
                    UpdateHeroSizing();
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

NextendoAccountDialog::~NextendoAccountDialog() {
    hid_core.SetGuestInputSuspended(false);
}

bool NextendoAccountDialog::eventFilter(QObject* watched, QEvent* event) {
    const bool is_activate_key =
        event->type() == QEvent::KeyPress &&
        (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space ||
         static_cast<QKeyEvent*>(event)->key() == Qt::Key_Return ||
         static_cast<QKeyEvent*>(event)->key() == Qt::Key_Enter);

    if (watched == header_code &&
        (event->type() == QEvent::MouseButtonRelease || is_activate_key)) {
        QApplication::clipboard()->setText(header_code->text());
        status->setText(tr("Friend code copied."));
        return true;
    }
    if (watched == header_avatar &&
        (event->type() == QEvent::MouseButtonRelease || is_activate_key)) {
        OnChangeAvatar();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void NextendoAccountDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_PageDown) {
        GoToPage(current_page + 1);
        return;
    }
    if (event->key() == Qt::Key_PageUp) {
        GoToPage(current_page - 1);
        return;
    }
    QDialog::keyPressEvent(event);
}

void NextendoAccountDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    UpdateHeroSizing();
}

void NextendoAccountDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (QApplication::focusWidget() && isAncestorOf(QApplication::focusWidget())) {
        return;
    }
    if (current_page == 0 && home_default_focus) {
        home_default_focus->setFocus();
    } else {
        focusNextChild();
    }
}

void NextendoAccountDialog::UpdateHeroSizing() {
    if (!header_card || !header_avatar) {
        return;
    }
    const int card_h = std::max(header_card->height(), 1);
    const int avatar_size = std::clamp(static_cast<int>(card_h * 0.62), 88, 260);
    header_avatar->setFixedSize(avatar_size, avatar_size);
    header_avatar->setPixmap(RoundedPixmap(header_avatar_source, avatar_size));

    QFont name_font = header_name->font();
    name_font.setPointSizeF(std::clamp(avatar_size * 0.19, 15.0, 34.0));
    name_font.setBold(true);
    header_name->setFont(name_font);

    QFont code_font(QStringLiteral("monospace"));
    code_font.setPointSizeF(std::clamp(avatar_size * 0.105, 10.0, 18.0));
    header_code->setFont(code_font);
}

void NextendoAccountDialog::GoToPage(int index) {
    if (!pages_stack) {
        return;
    }
    index = std::clamp(index, 0, pages_stack->count() - 1);
    if (index == current_page) {
        return;
    }

    current_page = index;
    pages_stack->setCurrentIndex(index);
    if (page_title_label && index < page_titles.size()) {
        page_title_label->setText(page_titles[index]);
    }
    UpdatePageNav();
    if (index == 0 && home_default_focus) {
        home_default_focus->setFocus();
    } else if (index == kFriendsPage && friends_view) {
        friends_view->setFocus();
    } else {
        focusNextChild();
    }
}

void NextendoAccountDialog::WireControllerNav() {
    connect(controller_navigation, &ControllerNavigation::leftShoulderPressed, this,
            [this] { GoToPage(current_page - 1); });
    connect(controller_navigation, &ControllerNavigation::rightShoulderPressed, this,
            [this] { GoToPage(current_page + 1); });
    connect(controller_navigation, &ControllerNavigation::navigated, this,
            &NextendoAccountDialog::OnDirectionalNav);
    connect(controller_navigation, &ControllerNavigation::activated, this,
            &NextendoAccountDialog::OnControllerActivate, Qt::QueuedConnection);
    connect(controller_navigation, &ControllerNavigation::backPressed, this, [this] {
        if (active_popup) {
            active_popup->deleteLater();
            active_popup = nullptr;
            return;
        }
        reject();
    });
}

void NextendoAccountDialog::UpdatePageNav() {
    if (!nav_left_arrow || !nav_right_arrow || !pages_stack) {
        return;
    }
    const int last = pages_stack->count() - 1;
    nav_left_arrow->SetState(current_page > 0,
                             current_page > 0 ? page_titles[current_page - 1] : QString{});
    nav_right_arrow->SetState(current_page < last,
                              current_page < last ? page_titles[current_page + 1] : QString{});
}

void NextendoAccountDialog::OnDirectionalNav(int dx, int dy) {
    QWidget* focused = QApplication::focusWidget();
    if (focused && !isAncestorOf(focused)) {
        focused = nullptr;
    }

    if (auto* list = qobject_cast<QAbstractItemView*>(focused)) {
        if (dy != 0) {
            QKeyEvent key(QEvent::KeyPress, dy > 0 ? Qt::Key_Down : Qt::Key_Up, Qt::NoModifier);
            QApplication::sendEvent(list, &key);
        }
        return;
    }

    if (current_page == kFriendsPage) {
        // Add Friend / Search Friends are text fields, not part of the controller focus chain.
        if (friends_view) {
            friends_view->setFocus();
        }
        return;
    }

    const QList<QAbstractButton*> picker_buttons = cloud_save_picker_group->buttons();
    const int picker_index = picker_buttons.indexOf(qobject_cast<QAbstractButton*>(focused));
    if (picker_index >= 0) {
        if (dx > 0 && picker_index + 1 < picker_buttons.size()) {
            picker_buttons[picker_index + 1]->setFocus();
            return;
        }
        if (dx < 0 && picker_index > 0) {
            picker_buttons[picker_index - 1]->setFocus();
            return;
        }
        if (dy > 0 && cloud_save_download_button->isEnabled()) {
            cloud_save_download_button->setFocus();
            return;
        }
        return;
    }
    if (focused == cloud_save_download_button && dy < 0 && !picker_buttons.isEmpty()) {
        picker_buttons.first()->setFocus();
        return;
    }

    if (dy > 0 || dx > 0) {
        focusNextChild();
    } else if (dy < 0 || dx < 0) {
        focusPreviousChild();
    }
}

void NextendoAccountDialog::OnControllerActivate() {
    QWidget* focused = QApplication::focusWidget();
    if (!focused || !isAncestorOf(focused)) {
        return;
    }
    if (focused == history_view) {
        ActivateHistoryRow(history_view->currentIndex());
        return;
    }
    if (auto* list = qobject_cast<QListView*>(focused);
        list == friends_view || list == requests_view || list == outgoing_requests_view) {
        ActivateCurrentRow(list);
        return;
    }
    if (auto* button = qobject_cast<QAbstractButton*>(focused)) {
        button->click();
        return;
    }
    if (auto* combo = qobject_cast<QComboBox*>(focused)) {
        combo->showPopup();
        return;
    }
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(focused, &key);
}

bool NextendoAccountDialog::ConfirmAction(const QString& title, const QString& message,
                                          const QString& yes_text, const QString& no_text,
                                          const std::string& icon_base64) {
    auto* scrim = new QWidget(this);
    scrim->setGeometry(rect());
    scrim->setStyleSheet(QStringLiteral("background: rgba(0,0,0,140);"));

    auto* card = new QFrame(scrim);
    card->setObjectName(QStringLiteral("confirmCard"));
    card->setFixedWidth(460);
    card->setStyleSheet(QStringLiteral(
        "QFrame#confirmCard { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
        "  stop:0 rgba(32, 32, 38, 250), stop:1 rgba(22, 22, 27, 255)); "
        "  border: 1px solid rgba(255,255,255,40); border-radius: 20px; }"));

    auto* title_label = new QLabel(title);
    QFont title_font = title_label->font();
    title_font.setBold(true);
    title_font.setPointSize(title_font.pointSize() + 2);
    title_label->setFont(title_font);
    title_label->setStyleSheet(QStringLiteral("color: #ffffff; background: transparent;"));
    title_label->setAlignment(Qt::AlignCenter);

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(32, 24, 32, 24);
    card_layout->setSpacing(14);
    card_layout->addWidget(title_label);

    if (!icon_base64.empty()) {
        QPixmap icon_source;
        icon_source.loadFromData(QByteArray::fromBase64(QByteArray::fromStdString(icon_base64)));
        if (!icon_source.isNull()) {
            auto* icon_label = new QLabel;
            icon_label->setFixedSize(88, 88);
            icon_label->setPixmap(RoundedRectPixmap(icon_source, 88, 12));
            icon_label->setAlignment(Qt::AlignCenter);
            card_layout->addWidget(icon_label, 0, Qt::AlignHCenter);
        }
    }

    auto* message_label = new QLabel(message);
    message_label->setWordWrap(true);
    message_label->setAlignment(Qt::AlignCenter);
    message_label->setStyleSheet(QStringLiteral("color: #ffffff; font-weight: 500; background: transparent;"));
    card_layout->addWidget(message_label);

    const QString button_style =
        QStringLiteral("QPushButton { background: rgba(255,255,255,10); "
                       "border: 1px solid rgba(255,255,255,25); border-radius: 14px; "
                       "color: #ffffff; padding: 10px 24px; }"
                       "QPushButton:hover { background: rgba(255,255,255,20); "
                       "border-color: rgba(255,255,255,60); }"
                       "QPushButton:focus { background: %1; border-color: %1; "
                       "color: #000000; font-weight: bold; }")
            .arg(AccentColor().name());

    auto* no_button = new QPushButton(no_text);
    no_button->setStyleSheet(button_style);
    auto* yes_button = new QPushButton(yes_text);
    yes_button->setStyleSheet(button_style);

    auto* button_row = new QHBoxLayout;
    button_row->setSpacing(12);
    button_row->addWidget(no_button);
    button_row->addWidget(yes_button);
    card_layout->addSpacing(4);
    card_layout->addLayout(button_row);

    card->adjustSize();
    card->move((scrim->width() - card->width()) / 2, (scrim->height() - card->height()) / 2);
    auto* scrim_opacity = new QGraphicsOpacityEffect(scrim);
    scrim_opacity->setOpacity(0.0);
    scrim->setGraphicsEffect(scrim_opacity);
    scrim->show();
    scrim->raise();
    no_button->setFocus();

    auto* fade_in = new QPropertyAnimation(scrim_opacity, "opacity", scrim);
    fade_in->setDuration(150);
    fade_in->setStartValue(0.0);
    fade_in->setEndValue(1.0);
    fade_in->setEasingCurve(QEasingCurve::OutCubic);
    fade_in->start(QAbstractAnimation::DeleteWhenStopped);

    QEventLoop loop;
    bool result = false;
    connect(yes_button, &QPushButton::clicked, &loop, [&] {
        result = true;
        loop.quit();
    });
    connect(no_button, &QPushButton::clicked, &loop, [&] { loop.quit(); });

    disconnect(controller_navigation, nullptr, this, nullptr);
    const auto nav_conn = connect(controller_navigation, &ControllerNavigation::navigated, &loop,
                                  [this](int dx, int) {
                                      if (dx > 0) {
                                          focusNextChild();
                                      } else if (dx < 0) {
                                          focusPreviousChild();
                                      }
                                  });
    const auto activate_conn =
        connect(controller_navigation, &ControllerNavigation::activated, &loop, [] {
            if (auto* button = qobject_cast<QAbstractButton*>(QApplication::focusWidget())) {
                button->click();
            }
        });
    const auto back_conn = connect(controller_navigation, &ControllerNavigation::backPressed,
                                   &loop, [&loop] { loop.quit(); });

    loop.exec();

    disconnect(nav_conn);
    disconnect(activate_conn);
    disconnect(back_conn);
    WireControllerNav();

    QEventLoop fade_loop;
    auto* fade_out = new QPropertyAnimation(scrim_opacity, "opacity", scrim);
    fade_out->setDuration(130);
    fade_out->setStartValue(1.0);
    fade_out->setEndValue(0.0);
    fade_out->setEasingCurve(QEasingCurve::InCubic);
    connect(fade_out, &QPropertyAnimation::finished, &fade_loop, &QEventLoop::quit);
    fade_out->start(QAbstractAnimation::DeleteWhenStopped);
    fade_loop.exec();

    scrim->deleteLater();
    return result;
}

void NextendoAccountDialog::ActivateCurrentRow(QListView* view) {
    if (view != friends_view && view != requests_view && view != outgoing_requests_view) {
        return;
    }

    const QModelIndex index = view->currentIndex();
    if (!index.isValid()) {
        return;
    }

    if (view == outgoing_requests_view) {
        const std::string code =
            index.data(NextendoFriendItem::FriendCodeRole).toString().toStdString();
        Common::NextendoOutgoingRequests::Remove(code);
        outgoing_requests_model->removeRow(index.row());
        outgoing_requests_section->setVisible(outgoing_requests_model->rowCount() > 0);
        return;
    }

    const bool is_request = index.data(NextendoFriendItem::IsRequestRole).toBool();
    const u64 pid = SelectedPid(index);
    if (pid == 0) {
        return;
    }

#ifdef ENABLE_WEB_SERVICE
    const std::string avatar_b64 = index.data(NextendoFriendItem::AvatarB64Role).toString().toStdString();
    if (is_request) {
        const QString name = index.data(NextendoFriendItem::NameRole).toString();
        if (!ConfirmAction(tr("Friend Request"), tr("Accept %1's friend request?").arg(name),
                          tr("Yes"), tr("No"), avatar_b64)) {
            return;
        }
        RunAsync([pid] { return WebService::NextendoApi::AcceptFriend(pid); });
    } else {
        const QString name = index.data(NextendoFriendItem::NameRole).toString();
        if (!ConfirmAction(tr("Remove Friend"), tr("Remove %1 from your friends list?").arg(name),
                          tr("Yes"), tr("No"), avatar_b64)) {
            return;
        }
        RunAsync([pid] { return WebService::NextendoApi::RemoveFriend(pid); });
    }
#endif
}

void NextendoAccountDialog::ActivateHistoryRow(const QModelIndex& index) {
    if (!index.isValid() || !controller) {
        return;
    }
    const QString title_id_hex = index.data(NextendoHistoryItem::TitleIdRole).toString();
    const QString name = index.data(NextendoHistoryItem::NameRole).toString();
    const std::string icon_b64 = index.data(NextendoHistoryItem::IconB64Role).toString().toStdString();
    bool ok = false;
    const u64 title_id = title_id_hex.toULongLong(&ok, 16);
    if (!ok || title_id == 0) {
        return;
    }
    if (!ConfirmAction(tr("Quick Start"), tr("Start %1?").arg(name), tr("Yes"), tr("No"), icon_b64)) {
        return;
    }
    controller->QuickStart(title_id);
    accept();
}

void NextendoAccountDialog::OnHistoryViewClicked(const QModelIndex& index) {
    ActivateHistoryRow(index);
}

void NextendoAccountDialog::OnChangeAvatar() {
#ifdef ENABLE_WEB_SERVICE
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a profile picture"),
                                                       QString{}, tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }

    QImage image(path);
    if (image.isNull()) {
        status->setText(tr("Couldn't read that image."));
        return;
    }
    const int side = std::min(image.width(), image.height());
    const QRect crop_rect((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    const QImage square = image.copy(crop_rect).scaled(256, 256, Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation);

    QByteArray jpeg_bytes;
    QBuffer buffer(&jpeg_bytes);
    buffer.open(QIODevice::WriteOnly);
    square.save(&buffer, "JPG", 85);
    const std::string image_base64 = jpeg_bytes.toBase64().toStdString();

    header_avatar_source = QPixmap::fromImage(square);
    UpdateHeroSizing();
    status->setText(tr("Uploading profile picture..."));

    std::thread{[this, image_base64, guard = QPointer<NextendoAccountDialog>(this)] {
        const std::string error = WebService::NextendoApi::PushProfilePicture(image_base64);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error] {
                if (!guard) {
                    return;
                }
                status->setText(error.empty() ? tr("Profile picture updated.")
                                             : QString::fromStdString(error));
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    status->setText(tr("This build has no web services support."));
#endif
}

void NextendoAccountDialog::OnChangeBackground() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a background image"),
                                                       QString{}, tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }

    QImage image(path);
    if (image.isNull()) {
        status->setText(tr("Couldn't read that image."));
        return;
    }
    if (image.width() > 1600 || image.height() > 900) {
        image = image.scaled(1600, 900, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    const auto out_path = BackgroundImagePath();
    if (!image.save(QString::fromStdString(out_path.string()), "PNG")) {
        status->setText(tr("Couldn't save that background image."));
        return;
    }

    ApplyBackground(QPixmap::fromImage(image));
    status->setText(tr("Background updated."));
}

void NextendoAccountDialog::OnRemoveBackground() {
    std::error_code ec;
    std::filesystem::remove(BackgroundImagePath(), ec);
    ApplyBackground({});
    status->setText(tr("Background removed."));
}

void NextendoAccountDialog::ApplyBackground(const QPixmap& pixmap) {
    static_cast<HeaderCard*>(header_card)->SetBackgroundImage(pixmap);
}

void NextendoAccountDialog::LoadSavedBackground() {
    const auto path = BackgroundImagePath();
    if (!std::filesystem::exists(path)) {
        return;
    }
    QPixmap pixmap(QString::fromStdString(path.string()));
    if (!pixmap.isNull()) {
        ApplyBackground(pixmap);
    }
}

void NextendoAccountDialog::OnEditUsername() {
#ifdef ENABLE_WEB_SERVICE
    static const QRegularExpression valid_name(QStringLiteral("^[A-Za-z0-9_-]{3,16}$"));

    bool ok = false;
    const QString name = QInputDialog::getText(
                             this, tr("Change Username"),
                             tr("3-16 characters: letters, digits, '_' or '-'"), QLineEdit::Normal,
                             header_name->text(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == header_name->text()) {
        return;
    }
    if (!valid_name.match(name).hasMatch()) {
        status->setText(tr("Invalid username."));
        return;
    }

    status->setText(tr("Updating username..."));
    const std::string new_name = name.toStdString();

    std::thread{[this, new_name, guard = QPointer<NextendoAccountDialog>(this)] {
        const std::string error = WebService::NextendoApi::SetUsername(new_name);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error, new_name] {
                if (!guard) {
                    return;
                }
                if (error.empty()) {
                    Common::NextendoAccount::Save(Common::NextendoAccount::GetPid(), new_name,
                                                  Common::NextendoAccount::GetFriendCode(),
                                                  Common::NextendoAccount::GetToken());
                    header_name->setText(QString::fromStdString(new_name));
                    status->setText(tr("Username updated."));
                } else {
                    status->setText(QString::fromStdString(error));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    status->setText(tr("This build has no web services support."));
#endif
}

void NextendoAccountDialog::SetBusy(bool busy) {
    add_button->setEnabled(!busy);
}

u64 NextendoAccountDialog::SelectedPid(const QModelIndex& index) const {
    return index.isValid() ? index.data(NextendoFriendItem::PidRole).toULongLong() : 0;
}

void NextendoAccountDialog::ApplyFriendFilter(const QString& text) {
    for (int row = 0; row < friends_model->rowCount(); ++row) {
        const QString name =
            friends_model->index(row, 0).data(NextendoFriendItem::NameRole).toString();
        friends_view->setRowHidden(row, !text.isEmpty() && !name.contains(text, Qt::CaseInsensitive));
    }
}

void NextendoAccountDialog::UpdateRequestsBadge(int count) {
    if (count <= 0) {
        requests_badge->hide();
        UpdateDashboard();
        return;
    }
    requests_badge->setText(count > 99 ? QStringLiteral("99+") : QString::number(count));
    requests_badge->show();
    UpdateDashboard();
}

void NextendoAccountDialog::UpdateDashboard() {
    int online = 0;
    int in_game = 0;
    for (int row = 0; row < friends_model->rowCount(); ++row) {
        const int presence =
            friends_model->index(row, 0).data(NextendoFriendItem::PresenceRole).toInt();
        if (presence == 1) {
            ++online;
        } else if (presence == 2) {
            ++online;
            ++in_game;
        }
    }
    dash_friends_online_value->setText(QString::number(online));
    dash_in_game_value->setText(QString::number(in_game));

    const int request_count = requests_model->rowCount();
    if (request_count > 0) {
        dash_requests_badge->setText(request_count > 9 ? QStringLiteral("9+")
                                                        : QString::number(request_count));
        dash_requests_badge->show();
        const QString first_name =
            requests_model->index(0, 0).data(NextendoFriendItem::NameRole).toString();
        dash_requests_preview->setText(request_count == 1
                                           ? tr("%1 wants to be friends.").arg(first_name)
                                           : tr("%1 and %2 more.").arg(first_name).arg(request_count - 1));
    } else {
        dash_requests_badge->hide();
        dash_requests_preview->setText(tr("No pending requests"));
    }

    QLayoutItem* item;
    while ((item = dash_friends_list_layout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    const int preview_count = friends_model->rowCount();
    if (preview_count == 0) {
        dash_friends_list_layout->addWidget(MakeDimLabel(tr("No friends yet.")));
    }
    for (int row = 0; row < preview_count; ++row) {
        const QModelIndex index = friends_model->index(row, 0);
        const QString name = index.data(NextendoFriendItem::NameRole).toString();
        const int presence = index.data(NextendoFriendItem::PresenceRole).toInt();
        const QString game = index.data(NextendoFriendItem::GamePresenceRole).toString();
        const std::string avatar_b64 =
            index.data(NextendoFriendItem::AvatarB64Role).toString().toStdString();
        const u64 pid = index.data(NextendoFriendItem::PidRole).toULongLong();
        const bool is_me = index.data(NextendoFriendItem::IsMeRole).toBool();

        auto* avatar_label = new QLabel;
        avatar_label->setFixedSize(kPreviewAvatarSize, kPreviewAvatarSize);
        const QPixmap avatar =
            Nextendo::AvatarCache::Get(name.toStdString(), avatar_b64, kPreviewAvatarSize);
        avatar_label->setPixmap(RoundedPixmap(avatar, kPreviewAvatarSize));

        auto* name_label = new QLabel(name);
        QFont name_font = name_label->font();
        name_font.setBold(true);
        name_label->setFont(name_font);

        QString status_text;
        switch (presence) {
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
        auto* status_label = new QLabel(status_text);
        status_label->setStyleSheet(
            QStringLiteral("color: %1;").arg(PresenceColor(presence).name()));

        auto* name_col = new QVBoxLayout;
        name_col->setSpacing(0);
        name_col->addWidget(name_label);
        name_col->addWidget(status_label);

        auto* row_layout = new QHBoxLayout;
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->addWidget(avatar_label);
        row_layout->addLayout(name_col);
        row_layout->addStretch();

        auto* row_widget = new QWidget;
        row_widget->setLayout(row_layout);
        if (!is_me && pid != 0) {
            row_widget->setContextMenuPolicy(Qt::CustomContextMenu);
            const QString avatar_qb64 = QString::fromStdString(avatar_b64);
            connect(row_widget, &QWidget::customContextMenuRequested, this,
                   [this, row_widget, pid, name, avatar_qb64](const QPoint& pos) {
                       QMenu menu(this);
                       QAction* invite_action = menu.addAction(tr("Invite to Chat Room"));
                       connect(invite_action, &QAction::triggered, this,
                              [this, pid, name] { emit InviteToChatRequested(pid, name); });
                       QAction* report_action = menu.addAction(tr("Report Player..."));
                       connect(report_action, &QAction::triggered, this, [this, pid, name,
                                                                          avatar_qb64] {
                           OpenReportDialog(pid, name, avatar_qb64);
                       });
                       menu.exec(row_widget->mapToGlobal(pos));
                   });
        }
        dash_friends_list_layout->addWidget(row_widget);
    }
    dash_friends_list_layout->addStretch(1);
}

void NextendoAccountDialog::OnFriendsViewClicked(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    auto* view = qobject_cast<QListView*>(sender());
    if (view == outgoing_requests_view) {
        const QRect cell_rect = view->visualRect(index);
        const QPoint pos = view->viewport()->mapFromGlobal(QCursor::pos());
        const auto hit = outgoing_request_delegate->HitTestActions(cell_rect, pos, false);
        if (hit != NextendoFriendDelegate::ActionHit::None) {
            const std::string code =
                index.data(NextendoFriendItem::FriendCodeRole).toString().toStdString();
            Common::NextendoOutgoingRequests::Remove(code);
            outgoing_requests_model->removeRow(index.row());
            outgoing_requests_section->setVisible(outgoing_requests_model->rowCount() > 0);
        }
        return;
    }

    auto* delegate = view == requests_view ? request_delegate : friend_delegate;
    const bool is_request = index.data(NextendoFriendItem::IsRequestRole).toBool();

    const QRect cell_rect = view->visualRect(index);
    const QPoint pos = view->viewport()->mapFromGlobal(QCursor::pos());
    const auto hit = delegate->HitTestActions(cell_rect, pos, is_request);
    if (hit == NextendoFriendDelegate::ActionHit::None) {
        return;
    }

    const u64 pid = SelectedPid(index);
    if (pid == 0) {
        return;
    }

#ifdef ENABLE_WEB_SERVICE
    const std::string avatar_b64 = index.data(NextendoFriendItem::AvatarB64Role).toString().toStdString();
    if (is_request) {
        const QString name = index.data(NextendoFriendItem::NameRole).toString();
        if (hit == NextendoFriendDelegate::ActionHit::Primary) {
            if (!ConfirmAction(tr("Friend Request"), tr("Accept %1's friend request?").arg(name),
                              tr("Yes"), tr("No"), avatar_b64)) {
                return;
            }
            RunAsync([pid] { return WebService::NextendoApi::AcceptFriend(pid); });
        } else {
            if (!ConfirmAction(tr("Friend Request"), tr("Decline %1's friend request?").arg(name),
                              tr("Yes"), tr("No"), avatar_b64)) {
                return;
            }
            RunAsync([pid] { return WebService::NextendoApi::DeclineFriend(pid); });
        }
    } else {
        const QString name = index.data(NextendoFriendItem::NameRole).toString();
        if (!ConfirmAction(tr("Remove Friend"), tr("Remove %1 from your friends list?").arg(name),
                          tr("Yes"), tr("No"), avatar_b64)) {
            return;
        }
        RunAsync([pid] { return WebService::NextendoApi::RemoveFriend(pid); });
    }
#endif
}

void NextendoAccountDialog::RunAsync(std::function<std::string()> task,
                                     std::function<void()> on_success) {
#ifdef ENABLE_WEB_SERVICE
    SetBusy(true);
    status->setText(tr("Working..."));

    std::thread{[this, work = std::move(task), success_cb = std::move(on_success),
                guard = QPointer<NextendoAccountDialog>(this)] {
        const std::string result = work();
        if (!guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error = result, success_cb] {
                if (!guard) {
                    return;
                }
                SetBusy(false);
                if (error.empty()) {
                    RefreshFriends();
                    if (controller) {
                        controller->RefreshFriendCache();
                    }
                    if (success_cb) {
                        success_cb();
                    }
                } else {
                    status->setText(QString::fromStdString(error));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoAccountDialog::OnAdd() {
#ifdef ENABLE_WEB_SERVICE
    const std::string code = friend_code_input->text().trimmed().toStdString();
    if (code.empty()) {
        status->setText(tr("Enter a friend code first."));
        return;
    }
    friend_code_input->clear();
    RunAsync([code] { return WebService::NextendoApi::AddFriendByCode(code); }, [this, code] {
        Common::NextendoOutgoingRequests::Add(code);
        if (controller) {
            controller->NotifyFriendRequestSent(QString::fromStdString(code));
        }
    });
#endif
}

void NextendoAccountDialog::RefreshFriends() {
#ifdef ENABLE_WEB_SERVICE
    SetBusy(true);
    status->setText(tr("Loading..."));

    std::thread{[this, guard = QPointer<NextendoAccountDialog>(this)] {
        auto fetched = WebService::NextendoApi::GetFriends();
        if (!guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, list = std::move(fetched)]() mutable {
                if (!guard) {
                    return;
                }
                SetBusy(false);

                // Preserve current row/focus across the model rebuild below.
                const bool friends_had_focus = friends_view->hasFocus();
                const QModelIndex friends_prev = friends_view->currentIndex();
                const u64 friends_prev_pid =
                    friends_prev.isValid() ? friends_prev.data(NextendoFriendItem::PidRole).toULongLong() : 0;
                const bool requests_had_focus = requests_view->hasFocus();
                const QModelIndex requests_prev = requests_view->currentIndex();
                const u64 requests_prev_pid =
                    requests_prev.isValid() ? requests_prev.data(NextendoFriendItem::PidRole).toULongLong() : 0;
                const bool outgoing_had_focus = outgoing_requests_view->hasFocus();
                const QModelIndex outgoing_prev = outgoing_requests_view->currentIndex();
                const QString outgoing_prev_code =
                    outgoing_prev.isValid() ? outgoing_prev.data(NextendoFriendItem::NameRole).toString()
                                            : QString{};

                friends_model->clear();
                requests_model->clear();

                if (!list.ok) {
                    status->setText(QString::fromStdString(list.error));
                    return;
                }

                const std::string local_app_id = controller ? controller->GetLocalAppId() : std::string{};
                std::unordered_map<std::string, int> group_size;
                for (const auto& entry : list.friends) {
                    if (entry.presence_status != 0 && !entry.app_id.empty()) {
                        ++group_size[entry.app_id];
                    }
                }
                const auto rank = [&](const WebService::NextendoApi::Friend& f) -> int {
                    if (f.presence_status == 0) {
                        return static_cast<int>(group_size.size()) + 2;
                    }
                    if (f.app_id.empty()) {
                        return static_cast<int>(group_size.size()) + 1;
                    }
                    if (!local_app_id.empty() && f.app_id == local_app_id) {
                        return 0;
                    }
                    return 1; // refined below by group size, same tier is fine for a stable sort
                };
                std::stable_sort(list.friends.begin(), list.friends.end(),
                                 [&](const WebService::NextendoApi::Friend& a, const WebService::NextendoApi::Friend& b) {
                                     const int ra = rank(a);
                                     const int rb = rank(b);
                                     if (ra != rb) {
                                         return ra < rb;
                                     }
                                     if (ra == 1 && a.app_id != b.app_id) {
                                         return group_size[a.app_id] > group_size[b.app_id];
                                     }
                                     return a.name < b.name;
                                 });

                for (const auto& entry : list.friends) {
                    const QString game =
                        controller ? controller->ResolveGameName(entry.app_id, entry.app_name)
                                   : QString{};
                    friends_model->appendRow(new NextendoFriendItem(
                        entry.pid, QString::fromStdString(entry.name),
                        QString::fromStdString(entry.friend_code), entry.presence_status, game,
                        QString::fromStdString(entry.image_base64), false));
                }
                for (const auto& entry : list.requests) {
                    requests_model->appendRow(new NextendoFriendItem(
                        entry.pid, QString::fromStdString(entry.name),
                        QString::fromStdString(entry.friend_code), entry.presence_status,
                        QString{}, QString::fromStdString(entry.image_base64), true));
                }

                std::vector<std::string> accepted_codes;
                accepted_codes.reserve(list.friends.size());
                for (const auto& entry : list.friends) {
                    accepted_codes.push_back(entry.friend_code);
                }
                Common::NextendoOutgoingRequests::PruneAccepted(accepted_codes);

                outgoing_requests_model->clear();
                for (const auto& entry : Common::NextendoOutgoingRequests::Get()) {
                    outgoing_requests_model->appendRow(new NextendoFriendItem(
                        0, QString::fromStdString(entry.friend_code),
                        QString::fromStdString(entry.friend_code), 0, QString{}, QString{}, false,
                        tr("Cancel")));
                }
                outgoing_requests_section->setVisible(outgoing_requests_model->rowCount() > 0);

                status->setText(tr("%1 friend(s), %2 request(s).")
                                    .arg(list.friends.size())
                                    .arg(list.requests.size()));
                UpdateRequestsBadge(static_cast<int>(list.requests.size()));

                friends_stack->setCurrentIndex(friends_model->rowCount() > 0 ? 0 : 1);
                requests_stack->setCurrentIndex(requests_model->rowCount() > 0 ? 0 : 1);
                ApplyFriendFilter(friend_search->text());

                if (friends_had_focus) {
                    for (int row = 0; row < friends_model->rowCount(); ++row) {
                        const QModelIndex idx = friends_model->index(row, 0);
                        if (idx.data(NextendoFriendItem::PidRole).toULongLong() == friends_prev_pid) {
                            friends_view->setCurrentIndex(idx);
                            friends_view->setFocus();
                            break;
                        }
                    }
                }
                if (requests_had_focus) {
                    for (int row = 0; row < requests_model->rowCount(); ++row) {
                        const QModelIndex idx = requests_model->index(row, 0);
                        if (idx.data(NextendoFriendItem::PidRole).toULongLong() == requests_prev_pid) {
                            requests_view->setCurrentIndex(idx);
                            requests_view->setFocus();
                            break;
                        }
                    }
                }
                if (outgoing_had_focus) {
                    for (int row = 0; row < outgoing_requests_model->rowCount(); ++row) {
                        const QModelIndex idx = outgoing_requests_model->index(row, 0);
                        if (idx.data(NextendoFriendItem::NameRole).toString() == outgoing_prev_code) {
                            outgoing_requests_view->setCurrentIndex(idx);
                            outgoing_requests_view->setFocus();
                            break;
                        }
                    }
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    status->setText(tr("This build has no web services support."));
    SetBusy(false);
#endif
}

void NextendoAccountDialog::RefreshCloudSaveTab() {
    const std::string app_id_hex = controller ? controller->GetLocalAppId() : std::string{};
    const bool any_game_running = !app_id_hex.empty();

    u64 running_title_id = 0;
    if (any_game_running) {
        try {
            running_title_id = std::stoull(app_id_hex, nullptr, 16);
        } catch (const std::exception&) {
        }
    }
    const bool is_running_eligible =
        running_title_id != 0 && Nextendo::CompatibleTitles::Table().count(running_title_id);

    if (is_running_eligible) {
        cloud_save_icon->setVisible(true);
        cloud_save_title->setVisible(true);
        cloud_save_picker_container->setVisible(false);
        cloud_save_download_button->setVisible(false);

        const QString game_name = controller->ResolveGameName(app_id_hex);
        const QString game_icon = controller->ResolveGameIcon(app_id_hex);
        cloud_save_title->setText(game_name);
        const QPixmap icon = RoundedRectPixmap(
            Nextendo::AvatarCache::Get(app_id_hex, game_icon.toStdString(), 64), 64, 10);
        if (!icon.isNull()) {
            cloud_save_icon->setPixmap(icon);
        } else {
            cloud_save_icon->clear();
        }
        cloud_save_status->setText(tr("Save uploads automatically when you stop this game."));
        return;
    }

    cloud_save_icon->setVisible(false);
    cloud_save_title->setVisible(false);

    if (any_game_running) {
        // A DIFFERENT (non-eligible) game is running. Downloading some other title's save
        // into its NAND save directory while emulation is active isn't safe -- the running
        // game may have that filesystem layer open -- so lock out the whole picker here too,
        // not just the button for the running title.
        cloud_save_download_button->setVisible(false);
        cloud_save_picker_container->setVisible(false);
        cloud_save_status->setText(tr("Stop the running game to download a cloud save."));
        return;
    }

    cloud_save_download_button->setVisible(true);
    cloud_save_picker_container->setVisible(true);
    RebuildCloudSaveTitlePicker();

    if (!cloud_save_probing.empty()) {
        cloud_save_status->setText(tr("Checking for cloud saves..."));
    } else if (cloud_save_picker_row->count() > 0) {
        cloud_save_status->setText(tr("Pick a title to download its cloud save."));
    } else {
        cloud_save_status->setText(tr("No cloud saves found for your installed games."));
    }
}

void NextendoAccountDialog::RebuildCloudSaveTitlePicker() {
    for (QAbstractButton* button : cloud_save_picker_group->buttons()) {
        cloud_save_picker_group->removeButton(button);
        button->deleteLater();
    }
    QLayoutItem* item;
    while ((item = cloud_save_picker_row->takeAt(0)) != nullptr) {
        delete item;
    }

    bool selection_still_valid = false;
    for (const auto& [program_id, version] : Nextendo::CompatibleTitles::Table()) {
        const std::string title_id_hex = fmt::format("{:016X}", program_id);
        const QString name = controller->ResolveGameName(title_id_hex);
        const QString icon_b64 = controller->ResolveGameIcon(title_id_hex);
        if (icon_b64.isEmpty()) {
            continue; // Not installed locally -- nothing to show for it.
        }

        const auto has_data_it = cloud_save_has_data.find(program_id);
        if (has_data_it == cloud_save_has_data.end()) {
            ProbeCloudSaveAvailability(program_id);
            continue; // Don't show until we actually know there's a cloud save.
        }
        if (!has_data_it->second) {
            continue;
        }

        auto* button = new QToolButton;
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(48, 48));
        button->setIcon(RoundedRectPixmap(
            Nextendo::AvatarCache::Get(title_id_hex, icon_b64.toStdString(), 48), 48, 8));
        button->setText(QFontMetrics(button->font()).elidedText(name, Qt::ElideRight, 92));
        button->setToolTip(name);
        button->setCursor(Qt::PointingHandCursor);
        button->setAutoRaise(true);
        button->setFixedWidth(104);
        button->setFocusPolicy(Qt::StrongFocus);
        if (program_id == cloud_save_selected_title_id) {
            button->setChecked(true);
            selection_still_valid = true;
        }
        connect(button, &QToolButton::clicked, this, [this, program_id] {
            cloud_save_selected_title_id = program_id;
            cloud_save_download_button->setEnabled(true);
        });

        cloud_save_picker_group->addButton(button);
        cloud_save_picker_row->addWidget(button);
    }

    if (!selection_still_valid) {
        cloud_save_selected_title_id = 0;
    }
    cloud_save_download_button->setEnabled(cloud_save_selected_title_id != 0);
}

void NextendoAccountDialog::ProbeCloudSaveAvailability(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    if (cloud_save_probing.count(title_id)) {
        return;
    }
    cloud_save_probing.insert(title_id);

    const std::string title_id_hex = fmt::format("{:016x}", title_id);
    std::thread{[this, title_id, title_id_hex, guard = QPointer<NextendoAccountDialog>(this)] {
        const auto save = WebService::NextendoApi::PullSave(title_id_hex);
        const bool has_data = save.has_value() && !save->empty();

        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, title_id, has_data] {
                if (!guard) {
                    return;
                }
                cloud_save_probing.erase(title_id);
                cloud_save_has_data[title_id] = has_data;
                RefreshCloudSaveTab();
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

namespace {
QString PlayerDisplayName(const WebService::NextendoApi::LobbyPlayer& player) {
    if (!player.name.empty()) {
        return QString::fromStdString(player.name);
    }
    return QStringLiteral("#%1").arg(player.pid);
}

QString LobbyStateLine(const WebService::NextendoApi::Lobby& lobby) {
    const QString status = lobby.state_code == "searching" ? NextendoAccountDialog::tr("Looking for players")
                          : lobby.state_code == "matched"  ? NextendoAccountDialog::tr("In a match")
                                                            : QString();
    if (status.isEmpty()) {
        return NextendoAccountDialog::tr("%1 / %2 players").arg(lobby.count).arg(lobby.max);
    }
    return NextendoAccountDialog::tr("%1 / %2 players \xE2\x80\x94 %3")
        .arg(lobby.count)
        .arg(lobby.max)
        .arg(status);
}
} // namespace

void NextendoAccountDialog::RefreshPlayers() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, guard = QPointer<NextendoAccountDialog>(this)] {
        auto lobby = WebService::NextendoApi::GetMyLobby();
        auto recent = WebService::NextendoApi::GetRecentPlayers();

        std::unordered_map<u64, std::string> avatars;
        const auto fetch_avatar = [&](u64 pid) {
            if (pid == 0 || avatars.contains(pid)) {
                return;
            }
            avatars[pid] = WebService::NextendoApi::GetAvatarByPid(pid);
        };
        for (const auto& player : lobby.players) {
            fetch_avatar(player.pid);
        }
        for (const auto& player : recent) {
            fetch_avatar(player.pid);
        }

        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, lobby = std::move(lobby), recent = std::move(recent),
             avatars = std::move(avatars)]() mutable {
                if (!guard) {
                    return;
                }

                known_player_pids.clear();
                lobby_model->clear();
                recent_players_model->clear();

                lobby_state_label->setText(lobby.in_lobby ? LobbyStateLine(lobby)
                                                          : tr("Not in a lobby."));
                for (const auto& player : lobby.players) {
                    if (player.known) {
                        known_player_pids.insert(player.pid);
                    }
                    lobby_model->appendRow(new NextendoFriendItem(
                        player.pid, PlayerDisplayName(player),
                        QString::fromStdString(player.friend_code), player.is_me ? 0 : 2,
                        controller ? controller->ResolveGameName(player.title_id) : QString{},
                        QString::fromStdString(avatars[player.pid]), false, tr("Add"),
                        player.is_me));
                }
                for (const auto& player : recent) {
                    if (player.known) {
                        known_player_pids.insert(player.pid);
                    }
                    recent_players_model->appendRow(new NextendoFriendItem(
                        player.pid, PlayerDisplayName(player),
                        QString::fromStdString(player.friend_code), 0,
                        controller ? controller->ResolveGameName(player.title_id) : QString{},
                        QString::fromStdString(avatars[player.pid]), false, tr("Add")));
                }

                lobby_stack->setCurrentIndex(lobby_model->rowCount() > 0 ? 0 : 1);
                recent_players_stack->setCurrentIndex(recent_players_model->rowCount() > 0 ? 0 : 1);
            });
    }}.detach();
#endif
}

void NextendoAccountDialog::OnPlayersViewClicked(const QModelIndex& index) {
#ifdef ENABLE_WEB_SERVICE
    if (!index.isValid()) {
        return;
    }
    auto* view = qobject_cast<QListView*>(sender());
    auto* delegate = view == lobby_view ? lobby_delegate : recent_players_delegate;

    const QRect cell_rect = view->visualRect(index);
    const QPoint pos = view->viewport()->mapFromGlobal(QCursor::pos());
    if (delegate->HitTestActions(cell_rect, pos, false) == NextendoFriendDelegate::ActionHit::None) {
        return;
    }

    const u64 pid = SelectedPid(index);
    if (pid == 0 || pid == Common::NextendoAccount::GetPid()) {
        return;
    }
    if (!known_player_pids.contains(pid)) {
        status->setText(tr("This player isn't a known Nextendo account."));
        return;
    }
    const std::string friend_code = index.data(NextendoFriendItem::FriendCodeRole).toString().toStdString();
    if (friend_code.empty()) {
        status->setText(tr("This player doesn't have a friend code available."));
        return;
    }
    const QString name = index.data(NextendoFriendItem::NameRole).toString();
    const std::string avatar_b64 = index.data(NextendoFriendItem::AvatarB64Role).toString().toStdString();
    if (!ConfirmAction(tr("Add Friend"), tr("Send %1 a friend request?").arg(name), tr("Yes"),
                       tr("No"), avatar_b64)) {
        return;
    }
    RunAsync([friend_code] { return WebService::NextendoApi::AddFriendByCode(friend_code); },
             [this, friend_code] {
                 Common::NextendoOutgoingRequests::Add(friend_code);
                 if (controller) {
                     controller->NotifyFriendRequestSent(QString::fromStdString(friend_code));
                 }
             });
#endif
}

void NextendoAccountDialog::ShowPlayersContextMenu(QListView* view, const QPoint& pos) {
#ifdef ENABLE_WEB_SERVICE
    const QModelIndex index = view->indexAt(pos);
    if (!index.isValid()) {
        return;
    }
    const u64 pid = SelectedPid(index);
    if (pid == 0 || pid == Common::NextendoAccount::GetPid() || !known_player_pids.contains(pid)) {
        return;
    }

    const QString name = index.data(NextendoFriendItem::NameRole).toString();
    const QString avatar_b64 = index.data(NextendoFriendItem::AvatarB64Role).toString();

    QMenu menu(this);
    QAction* report_action = menu.addAction(tr("Report Player..."));
    connect(report_action, &QAction::triggered, this,
           [this, pid, name, avatar_b64] { OpenReportDialog(pid, name, avatar_b64); });
    menu.exec(view->viewport()->mapToGlobal(pos));
#endif
}

void NextendoAccountDialog::ShowFriendsContextMenu(const QPoint& pos) {
    const QModelIndex index = friends_view->indexAt(pos);
    if (!index.isValid()) {
        return;
    }
    const bool is_me = index.data(NextendoFriendItem::IsMeRole).toBool();
    const u64 pid = SelectedPid(index);
    if (pid == 0 || is_me) {
        return;
    }
    const QString name = index.data(NextendoFriendItem::NameRole).toString();
    const QString avatar_b64 = index.data(NextendoFriendItem::AvatarB64Role).toString();

    QMenu menu(this);
    QAction* invite_action = menu.addAction(tr("Invite to Chat Room"));
    connect(invite_action, &QAction::triggered, this,
           [this, pid, name] { emit InviteToChatRequested(pid, name); });
    QAction* report_action = menu.addAction(tr("Report Player..."));
    connect(report_action, &QAction::triggered, this,
           [this, pid, name, avatar_b64] { OpenReportDialog(pid, name, avatar_b64); });
    menu.exec(friends_view->viewport()->mapToGlobal(pos));
}

void NextendoAccountDialog::OpenReportDialog(u64 pid, const QString& name,
                                             const QString& avatar_b64) {
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
    reason_combo->addItem(tr("Impersonating a Nextendo account"),
                          QStringLiteral("impersonation"));
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

    RunAsync(
        [pid, reason, comment] {
            const std::string error = WebService::NextendoApi::ReportPlayer(pid, reason, comment);
            if (error == "not_encountered") {
                return std::string("You haven't shared a lobby with this player recently.");
            }
            if (error == "quota") {
                return std::string("You've sent too many reports recently. Try again later.");
            }
            return error;
        },
        [this] { status->setText(tr("Report sent.")); });
#endif
}

void NextendoAccountDialog::RefreshHistory() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, guard = QPointer<NextendoAccountDialog>(this)] {
        auto fetched = WebService::NextendoApi::GetHistory();
        if (!fetched.ok || !guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, list = std::move(fetched)] {
                if (!guard) {
                    return;
                }
                const bool history_had_focus = history_view->hasFocus();
                const QModelIndex history_prev = history_view->currentIndex();
                const QString history_prev_title =
                    history_prev.isValid() ? history_prev.data(NextendoHistoryItem::TitleIdRole).toString()
                                           : QString{};

                history_model->clear();
                for (const auto& entry : list.entries) {
                    const QString local_name =
                        controller ? controller->ResolveGameName(entry.title_id) : QString{};
                    const QString local_icon =
                        controller ? controller->ResolveGameIcon(entry.title_id) : QString{};
                    const QString name = (!local_name.isEmpty() && local_name != tr("a game"))
                                             ? local_name
                                             : QString::fromStdString(entry.name);
                    if (name.contains(QStringLiteral(".nca"), Qt::CaseInsensitive)) {
                        continue;
                    }
                    const QString icon =
                        !local_icon.isEmpty() ? local_icon : QString::fromStdString(entry.icon_base64);
                    history_model->appendRow(new NextendoHistoryItem(
                        QString::fromStdString(entry.title_id), name, icon, entry.seconds,
                        QString::fromStdString(entry.last_played)));
                }
                history_stack->setCurrentIndex(history_model->rowCount() > 0 ? 0 : 1);

                if (history_had_focus) {
                    for (int row = 0; row < history_model->rowCount(); ++row) {
                        const QModelIndex idx = history_model->index(row, 0);
                        if (idx.data(NextendoHistoryItem::TitleIdRole).toString() == history_prev_title) {
                            history_view->setCurrentIndex(idx);
                            history_view->setFocus();
                            break;
                        }
                    }
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}
