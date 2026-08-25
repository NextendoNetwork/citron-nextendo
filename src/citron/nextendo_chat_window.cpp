// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_chat_window.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

NextendoChatWindow::NextendoChatWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Chat Room"));
    resize(340, 200);

    auto* layout = new QVBoxLayout(this);
    layout->addStretch();

    auto* create_button = new QPushButton(tr("Create a Room"), this);
    create_button->setMinimumHeight(40);
    connect(create_button, &QPushButton::clicked, this,
            [this] { emit CreateRoomRequested(); });
    layout->addWidget(create_button);

    layout->addSpacing(16);
    auto* join_label = new QLabel(tr("Or join with a room code:"), this);
    join_label->setAlignment(Qt::AlignHCenter);
    layout->addWidget(join_label);

    auto* join_row = new QWidget(this);
    auto* join_row_layout = new QHBoxLayout(join_row);
    join_row_layout->setContentsMargins(0, 0, 0, 0);
    join_code_input = new QLineEdit(join_row);
    join_code_input->setPlaceholderText(tr("Room code"));
    join_code_input->setMaxLength(6);
    join_code_input->setAlignment(Qt::AlignCenter);
    QFont code_font = join_code_input->font();
    code_font.setLetterSpacing(QFont::AbsoluteSpacing, 3);
    code_font.setBold(true);
    join_code_input->setFont(code_font);
    join_row_layout->addWidget(join_code_input);
    auto* join_button = new QPushButton(tr("Join"), join_row);
    connect(join_button, &QPushButton::clicked, this, [this] {
        const QString code = join_code_input->text().trimmed().toUpper();
        if (!code.isEmpty()) {
            emit JoinRoomRequested(code);
        }
    });
    connect(join_code_input, &QLineEdit::returnPressed, this, [this] {
        const QString code = join_code_input->text().trimmed().toUpper();
        if (!code.isEmpty()) {
            emit JoinRoomRequested(code);
        }
    });
    join_row_layout->addWidget(join_button);
    layout->addWidget(join_row);
    layout->addStretch();
}
