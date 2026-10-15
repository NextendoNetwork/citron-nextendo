// SPDX-License-Identifier: GPL-2.0-or-later
// Included by main.cpp so the assistant can reuse the existing installation flows.
void GMainWindow::OnSetupWizard() {
    if (system->IsPoweredOn()) {
        QMessageBox::information(this, tr("Setup Wizard"),
                                 tr("Stop emulation before opening the setup wizard."));
        return;
    }
    QWizard wizard(this);
    setup_wizard_window = &wizard;
    SCOPE_EXIT { setup_wizard_window = nullptr; };
    wizard.setWindowTitle(tr("Configure Citron"));
    wizard.setWizardStyle(QWizard::ModernStyle);
    wizard.setMinimumSize(720, 540);
    wizard.setButtonText(QWizard::CancelButton, tr("Continue later"));
    const auto page = [&](const QString& title, const QString& text) {
        auto* result = new QWizardPage(&wizard);
        result->setTitle(title);
        auto* layout = new QVBoxLayout(result);
        layout->setAlignment(Qt::AlignTop);
        auto* description = new QLabel(text, result);
        description->setWordWrap(true);
        layout->addWidget(description);
        wizard.addPage(result);
        return result;
    };
    const auto button = [&](QWizardPage* target, const QString& text, auto action) {
        auto* result = new QPushButton(text, target);
        target->layout()->addWidget(result);
        connect(result, &QPushButton::clicked, &wizard, action);
        return result;
    };
    page(tr("Everything in one place"),
         tr("Configure keys, firmware, games and updates, your controller, Internet, "
            "Nextendo, Discord activity, HOME shortcuts. Each step explains what "
            "it does. Your choices are saved; reopen this assistant from Tools > Setup Wizard."));

    auto* keys_page = page(tr("1. Decryption keys"),
        tr("Import prod.keys from your console dump so Citron can read your games and firmware. "
           "title.keys is also imported when it is in the same folder."));
    auto* keys_status = new QLabel(keys_page); keys_page->layout()->addWidget(keys_status);
    button(keys_page, tr("Install my keys..."), [&] { OnInstallDecryptionKeys(); });
    auto* firmware_page = page(tr("2. System firmware and HOME"),
        tr("Import your dumped firmware from a ZIP or folder. Install keys first. Firmware supplies "
           "the HOME menu (qlaunch) and system applets used by games."));
    auto* firmware_status = new QLabel(firmware_page); firmware_page->layout()->addWidget(firmware_status);
    auto* firmware_button = button(firmware_page, tr("Install my firmware..."), [&] { OnInstallFirmware(); });
    button(firmware_page, tr("Create a HOME shortcut..."), [&] { ui->action_Create_Home_Shortcut->trigger(); });
    auto* games_page = page(tr("3. Games, updates and DLC"),
        tr("Add folders containing your dumped XCI/NSP games; subfolders are included and changes "
           "are detected while HOME is open. Add folders containing your matching updates and DLC "
           "to load them directly from their current location. Subfolders are scanned too. "
           "Keep these folders available when playing. Updates and DLC are not downloaded by this assistant."));
    auto* games_status = new QLabel(games_page); games_page->layout()->addWidget(games_status);
    button(games_page, tr("Add a games folder..."), [&] { AddGameDirectory(true); });
    auto* content_status = new QLabel(games_page);
    games_page->layout()->addWidget(content_status);
    button(games_page, tr("Add updates/DLC folders..."), [&] {
        const auto selected = QFileDialog::getExistingDirectory(&wizard, tr("Select updates/DLC folder"));
        if (selected.isEmpty()) return;
        const QFileInfo info{selected};
        const auto path = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
        auto& folders = Settings::values.external_content_dirs;
        const bool duplicate = std::any_of(folders.begin(), folders.end(), [&](const auto& existing) {
            const QFileInfo old{QString::fromStdString(existing)};
            return (old.canonicalFilePath().isEmpty() ? old.absoluteFilePath() : old.canonicalFilePath()) == path;
        });
        if (duplicate) return;
        folders.push_back(path.toStdString());
        config->SaveAllValues();
        system->RefreshExternalContent();
        game_list->PopulateAsync(UISettings::values.game_dirs);
    });
    auto* input_page = page(tr("4. Controller"),
        tr("Connect your controller, choose it in Input, map buttons and check its input. "
           "You can also configure keyboard controls and the HOME button."));
    button(input_page, tr("Configure controller..."), [&] { setup_open_input = true; OnConfigure(); });

    auto* network_page = page(tr("5. Internet connection"),
        tr("Choose the PC's active network interface. HOME reads its connection type, Wi-Fi band, "
           "channel and addressing when available. Use HOME > System Settings > Internet > Test "
           "Connection to measure download/upload speed and record the last NAT result. "
           "Speed comes from a transfer test, not your computer model."));
    auto* network_status = new QLabel(network_page); network_status->setWordWrap(true);
    network_page->layout()->addWidget(network_status);
    auto* enable_internet = new QCheckBox(tr("Enable Internet for Citron"), network_page);
    enable_internet->setChecked(!Settings::values.airplane_mode.GetValue());
    network_page->layout()->addWidget(enable_internet);
    auto* interface_choice = new QComboBox(network_page);
    const auto populate_interfaces = [&] {
        const QSignalBlocker blocker{interface_choice};
        interface_choice->clear();
        interface_choice->addItem(tr("Automatic: use the PC's active connection"), QString{});
        for (const auto& iface : Network::GetAvailableNetworkInterfaces()) {
            interface_choice->addItem(tr("%1 (%2)")
                .arg(QString::fromStdString(iface.name), iface.wireless ? tr("Wi-Fi") : tr("Ethernet")),
                QString::fromStdString(iface.name));
        }
        const auto configured = QString::fromStdString(Settings::values.network_interface.GetValue());
        const int index = interface_choice->findData(configured);
        interface_choice->setCurrentIndex(index < 0 ? 0 : index);
    };
    populate_interfaces();
    interface_choice->setEnabled(enable_internet->isChecked());
    network_page->layout()->addWidget(interface_choice);
    connect(enable_internet, &QCheckBox::toggled, &wizard, [&, this](bool enabled) {
        Settings::values.airplane_mode.SetValue(!enabled);
        interface_choice->setEnabled(enabled);
        config->SaveAllValues();
    });
    connect(interface_choice, &QComboBox::currentIndexChanged, &wizard, [&, this](int) {
        Settings::values.network_interface.SetValue(interface_choice->currentData().toString().toStdString());
        config->SaveAllValues();
    });
    button(network_page, tr("Refresh Wi-Fi / Ethernet adapters"), populate_interfaces);
    button(network_page, tr("Configure Internet interface..."), [&] { setup_open_network = true; OnConfigure(); });
    auto* online_page = page(tr("6. Nextendo Network"),
        tr("Nextendo connects compatible games to its network. Sign in to use your profile and "
           "friend code, friends and their activity, game history, invitations and chat rooms. "
           "Game availability and online services depend on the supported title and server. "
           "Enable the connection below and finish sign-in in your browser; then return to this step."));
    auto* online_status = new QLabel(online_page); online_page->layout()->addWidget(online_status);
    auto* enable_online = new QCheckBox(tr("Connect compatible games to Nextendo"), online_page);
    enable_online->setChecked(ui->action_Nextendo_Enable_Redirection->isChecked());
    online_page->layout()->addWidget(enable_online);
    connect(enable_online, &QCheckBox::toggled, &wizard, [this](bool value) {
        ui->action_Nextendo_Enable_Redirection->setChecked(value); config->SaveAllValues();
    });
    auto* signin_status = new QLabel(online_page);
    signin_status->setWordWrap(true);
    online_page->layout()->addWidget(signin_status);
    connect(nextendo_controller, &NextendoController::StatusChanged, &wizard,
            [signin_status](const QString& message) { signin_status->setText(message); });
    auto* online_button = button(online_page, tr("Sign in to Nextendo..."), [&] {
        nextendo_controller->SignIn();
    });
    connect(nextendo_controller, &NextendoController::SignInFinished, &wizard, [&, online_button] {
        online_button->setEnabled(!Common::NextendoAccount::IsLinked());
    });
    button(online_page, tr("Open Nextendo account page"), [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://nextendo.network/compte")));
    });
    auto* discord_page = page(tr("7. Discord activity"),
        tr("Discord activity shows the current game or HOME menu, with a game image, in your "
           "Discord profile. Discord must be running. Choose whether to share that activity below."));
    auto* discord = new QCheckBox(tr("Show my Citron activity in Discord"), discord_page);
    discord->setChecked(setup_discord_action && setup_discord_action->isChecked());
    discord_page->layout()->addWidget(discord);
    connect(discord, &QCheckBox::toggled, &wizard, [this](bool value) {
        if (setup_discord_action) setup_discord_action->setChecked(value);
    });
    auto* finish_page = page(tr("Setup summary"),
        tr("Your installed files and choices are saved, including when you continue later. "
           "Use Tools > Load HOME Menu to start HOME. You can revisit any step with Back."));
    auto* tools_description = new QLabel(tr(
        "Finish prepares these official homebrew tools on the virtual SD card:\n"
        "Homebrew Menu: launch NRO applications.\n"
        "NXThemes Installer: apply themes. Themezer-NX: browse and download themes.\n"
        "Sphaira: browse files and homebrew applications. JKSV: back up and restore saves.\n"
        "Existing tools are preserved. Missing files use the included packages or official downloads.\n"
        "From HOME: Ctrl+H or L+R+Minus on Player 1. Customize this in Input > Homebrew Menu."), finish_page);
    tools_description->setWordWrap(true);
    finish_page->layout()->addWidget(tools_description);
    auto* summary = new QLabel(finish_page); summary->setWordWrap(true); finish_page->layout()->addWidget(summary);
    const auto refresh = [&] {
        const bool keys = ContentManager::AreKeysPresent();
        const bool firmware = CheckFirmwarePresence();
        keys_status->setText(keys ? tr("Keys: installed") : tr("Keys: missing"));
        firmware_status->setText(firmware ? tr("Firmware: installed") : tr("Firmware: missing"));
        firmware_button->setEnabled(keys);
        int folders = 0;
        for (const auto& dir : UISettings::values.game_dirs) {
            if (dir.path != "SDMC" && dir.path != "UserNAND" && dir.path != "SysNAND" &&
                QDir(QString::fromStdString(dir.path)).exists()) ++folders;
        }
        games_status->setText(tr("Available game folders: %1").arg(folders));
        content_status->setText(tr("Updates/DLC folders: %1")
            .arg(Settings::values.external_content_dirs.size()));
        const bool linked = Common::NextendoAccount::IsLinked();
        online_status->setText(linked ? tr("Nextendo: account linked") : tr("Nextendo: sign-in needed for account features"));
        online_button->setEnabled(!linked && !nextendo_controller->IsSigningIn());
        online_button->setText(nextendo_controller->IsSigningIn()
            ? tr("Waiting for browser sign-in...") : tr("Sign in to Nextendo..."));
        {
            const QSignalBlocker blocker{enable_internet};
            enable_internet->setChecked(!Settings::values.airplane_mode.GetValue());
            interface_choice->setEnabled(enable_internet->isChecked());
        }
        if (wizard.currentPage() == network_page) {
            if (const auto iface = Network::GetSelectedNetworkInterface()) {
                network_status->setText(tr("Selected interface: %1\nConnection: %2%3")
                    .arg(QString::fromStdString(iface->name), iface->wireless ? tr("Wi-Fi") : tr("Ethernet"),
                         iface->wireless && iface->frequency_khz ? tr(" / %1 GHz / channel %2")
                           .arg(iface->frequency_khz / 1000000.0, 0, 'f', 1).arg(iface->wireless_channel) : QString{}));
            } else network_status->setText(tr("No active interface available. Check the PC connection and airplane mode."));
        }
        summary->setText(keys_status->text() + QLatin1Char('\n') + firmware_status->text() +
            QLatin1Char('\n') + games_status->text() + QLatin1Char('\n') + content_status->text() +
            QLatin1Char('\n') + online_status->text() +
            QLatin1Char('\n') +
            tr("Internet: %1 / %2").arg(enable_internet->isChecked() ? tr("enabled") : tr("disabled"),
                interface_choice->currentText()) + QLatin1Char('\n') +
            tr("Discord activity: %1").arg(discord->isChecked() ? tr("enabled") : tr("disabled")) +
            QStringLiteral("\n\n") + (keys && firmware ? tr("HOME components are installed.") :
                tr("Setup is incomplete. Install the missing components before starting HOME.")));
    };
    connect(&wizard, &QWizard::currentIdChanged, &wizard, refresh);
    QTimer refresh_timer(&wizard);
    connect(&refresh_timer, &QTimer::timeout, &wizard, refresh);
    refresh_timer.start(2000);
    refresh();
    const auto result = wizard.exec();
    UISettings::values.setup_wizard_seen = true;
    UISettings::values.setup_resume = false;
    config->SaveAllValues();
    if (result == QDialog::Accepted) {
        HomebrewSetup::Prepare(this, [](const auto& zip, const auto& directory) {
            return ExtractZipToDirectory(zip, directory);
        });
    }
}
