// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <functional>
#include <utility>
#include <vector>
#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QGuiApplication>
#include <QListWidget>
#include <QMessageBox>
#include "citron/configuration/configuration_shared.h"
#include "citron/configuration/configure_general.h"
#include "citron/configuration/shared_widget.h"
#include "citron/uisettings.h"
#include "common/settings.h"
#include "core/core.h"
#include "ui_configure_general.h"

ConfigureGeneral::ConfigureGeneral(Core::System& system_,
                                   std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
                                   const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGeneral>()}, system{system_} {
    ui->setupUi(this);

    Setup(builder);

    SetConfiguration();

    connect(ui->button_reset_defaults, &QPushButton::clicked, this,
            &ConfigureGeneral::ResetDefaults);

    if (!Settings::IsConfiguringGlobal()) {
        ui->button_reset_defaults->setVisible(false);
    }

    connect(ui->button_add_external_content, &QPushButton::clicked, this,
            &ConfigureGeneral::AddExternalContentDir);
    connect(ui->button_remove_external_content, &QPushButton::clicked, this,
            &ConfigureGeneral::RemoveExternalContentDir);
    connect(ui->button_open_external_content, &QPushButton::clicked, this, [this] {
        const auto* item = ui->external_content_list->currentItem();
        if (!item)
            return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(item->text()));
    });
}

ConfigureGeneral::~ConfigureGeneral() = default;

void ConfigureGeneral::SetConfiguration() {
    RefreshExternalContentList();
}

void ConfigureGeneral::Setup(const ConfigurationShared::Builder& builder) {
    QLayout& general_layout = *ui->general_widget->layout();
    QLayout& linux_layout = *ui->linux_widget->layout();

    std::map<u32, QWidget*> general_hold{};
    std::map<u32, QWidget*> linux_hold{};

    std::vector<Settings::BasicSetting*> settings;

    auto push = [&settings](auto& list) {
        for (auto setting : list) {
            settings.push_back(setting);
        }
    };

    push(UISettings::values.linkage.by_category[Settings::Category::UiGeneral]);
    push(Settings::values.linkage.by_category[Settings::Category::Linux]);

// Only show Linux group on Unix
#ifndef __unix__
    ui->LinuxGroupBox->setVisible(false);
#endif

    for (const auto setting : settings) {
        auto* widget = builder.BuildWidget(setting, apply_funcs);

        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        switch (setting->GetCategory()) {
        case Settings::Category::UiGeneral:
            general_hold.emplace(setting->Id(), widget);
            break;
        case Settings::Category::Linux:
            linux_hold.emplace(setting->Id(), widget);
            break;
        default:
            widget->deleteLater();
        }
    }

    for (const auto& [id, widget] : general_hold) {
        general_layout.addWidget(widget);
    }
    for (const auto& [id, widget] : linux_hold) {
        linux_layout.addWidget(widget);
    }

// --- Manually add Wayland setting to the Linux UI group ---
#ifdef __linux__
    // This logic only runs if the user is on a Wayland session.
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        // Create a new, clean checkbox.
        auto wayland_checkbox = new QCheckBox(tr("Enable Wayland Performance Optimizations"));
        wayland_checkbox->setToolTip(tr("Use Wayland-specific presentation modes to reduce input "
                                        "latency and improve smoothness."));

        // Set its initial checked state from our hidden setting.
        wayland_checkbox->setChecked(Settings::values.is_wayland_platform.GetValue());

        // Connect the checkbox so it toggles our hidden setting.
        connect(wayland_checkbox, &QCheckBox::toggled, this,
                [](bool checked) { Settings::values.is_wayland_platform.SetValue(checked); });

        // Add our new, clean checkbox to the Linux layout.
        linux_layout.addWidget(wayland_checkbox);
    }
#endif
}

// Called to set the callback when resetting settings to defaults
void ConfigureGeneral::SetResetCallback(std::function<void()> callback) {
    reset_callback = std::move(callback);
}

void ConfigureGeneral::ResetDefaults() {
    QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("citron"),
        tr("This reset all settings and remove all per-game configurations. This will not delete "
           "game directories, profiles, or input profiles. Proceed?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::No) {
        return;
    }
    UISettings::values.reset_to_defaults = true;
    UISettings::values.is_game_list_reload_pending.exchange(true);
    reset_callback();
}

void ConfigureGeneral::ApplyConfiguration() {

    std::vector<std::string> new_external_dirs;
    for (int i = 0; i < ui->external_content_list->count(); ++i) {
        new_external_dirs.push_back(ui->external_content_list->item(i)->text().toStdString());
    }
    // Only rescan when the dirs actually changed -- this used to run unconditionally on every
    // Apply/OK regardless of which tab was touched, immediately followed by LoadROM's own
    // rescan of the same dirs at boot. Back-to-back scans of the same NSPs crash (dangling VFS
    // reference), so skip the redundant one instead of only papering over that symptom.
    const bool dirs_changed = new_external_dirs != Settings::values.external_content_dirs;
    Settings::values.external_content_dirs = std::move(new_external_dirs);

    if (dirs_changed) {
        system.RefreshExternalContent();
    }

    bool powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(powered_on);
    }
}

void ConfigureGeneral::RefreshExternalContentList() {
    ui->external_content_list->clear();
    for (const auto& dir : Settings::values.external_content_dirs) {
        ui->external_content_list->addItem(QString::fromStdString(dir));
    }
}

void ConfigureGeneral::AddExternalContentDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select Content Directory"));
    if (dir.isEmpty()) {
        return;
    }
    // Check if valid and not duplicate (optional but good UI)
    for (int i = 0; i < ui->external_content_list->count(); ++i) {
        if (ui->external_content_list->item(i)->text() == dir) {
            return;
        }
    }
    ui->external_content_list->addItem(dir);
}

void ConfigureGeneral::RemoveExternalContentDir() {
    qDeleteAll(ui->external_content_list->selectedItems());
}

void ConfigureGeneral::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGeneral::RetranslateUI() {
    ui->retranslateUi(this);
}
