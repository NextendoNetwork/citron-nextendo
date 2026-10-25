// SPDX-FileCopyrightText: Copyright 2026 Citron contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <QDialog>
#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
class QNetworkAccessManager;
class QNetworkReply;
class ControllerShortcut;

namespace Core::HID {
class EmulatedController;
}

class AmiiboPickerDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AmiiboPickerDialog(QWidget* parent, QString title_id,
                                Core::HID::EmulatedController* controller,
                                QString last_scanned_amiibo_id = {});
    ~AmiiboPickerDialog() override;

    [[nodiscard]] bool LoadBinRequested() const { return load_bin_requested; }
    [[nodiscard]] QString SelectedAmiiboId() const { return selected_id; }
    [[nodiscard]] QString SelectedAmiiboName() const { return selected_name; }

private:
    void LoadCatalog();
    void LoadFavorites();
    void SaveFavorites() const;
    void PopulateCategories();
    void FilterCatalog();
    void MoveSelection(int direction);
    void UpdateSelection(int row);
    void ToggleFavorite();
    void ScanSelectedAmiibo();
    void LoadLocalBinInstead();

    QString title_id;
    QString selected_id;
    QString selected_name;
    QString last_scanned_amiibo_id;
    QList<int> visible_entries;
    QJsonArray entries;
    QSet<QString> favorites;

    QLineEdit* search_box{};
    QListWidget* amiibo_list{};
    QCheckBox* show_all{};
    QComboBox* category_combo{};
    QLabel* image_label{};
    QLabel* details_label{};
    QPushButton* scan_button{};
    QPushButton* favorite_button{};
    QTimer* controller_repeat_timer{};
    Core::HID::EmulatedController* navigation_controller{};
    int navigation_callback_key{-1};
    std::atomic_bool navigation_up_pressed{};
    std::atomic_bool navigation_down_pressed{};
    QNetworkAccessManager* network{};
    QNetworkReply* image_reply{};
    std::vector<std::unique_ptr<ControllerShortcut>> controller_shortcuts;
    bool load_bin_requested{};
};