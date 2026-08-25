// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QWidget>

class QPropertyAnimation;
class QPaintEvent;
class QMouseEvent;

// Passive "<friend> is now playing <game>" popup, corner of the main window (user-configurable,
// see UISettings::values.nextendo_notification_corner). No invite action, dismisses on timeout
// or click. Only shown while main_window is active and not minimized -- see NextendoToast::Show.
class NextendoToast : public QWidget {
    Q_OBJECT

public:
    enum class Corner { TopRight, TopLeft, BottomRight, BottomLeft };
    enum class Kind { Online, Offline, Request, RequestSent, ChatRequest };

    explicit NextendoToast(QWidget* main_window);
    ~NextendoToast() override;

    void Show(const QString& headline, const QString& detail, const QString& avatar_base64,
             Kind kind = Kind::Online);

signals:
    void clicked(NextendoToast::Kind kind);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void Reposition();
    void HideAnimated();
    float ComputeScale() const;
    Corner CurrentCorner() const;
    QColor AccentColor() const;
    QString CategoryLabel() const;

    QWidget* main_window;
    QTimer auto_hide_timer;
    QPropertyAnimation* fade;
    QPropertyAnimation* slide;
    QPixmap avatar;
    QString line1;
    QString line2;
    Kind kind = Kind::Online;
    float scale = 1.0f; // recomputed per Show() from the current screen's resolution
};
