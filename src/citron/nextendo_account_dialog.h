// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <QDialog>
#include <QPixmap>
#include <QTimer>

#include "common/common_types.h"

class QAbstractButton;
class QButtonGroup;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QStackedWidget;
class QStandardItemModel;
class QModelIndex;
class QToolButton;
class QVBoxLayout;
class ControllerNavigation;
class NextendoController;
class NextendoFriendDelegate;
class NextendoNetworkProbe;
class NextendoPageNavArrow;

namespace Core {
class System;
}

namespace Core::HID {
class HIDCore;
}

class NextendoAccountDialog : public QDialog {
    Q_OBJECT

public:
    static constexpr int kHomePage = 0;
    static constexpr int kFriendsPage = 1; // also hosts incoming/outgoing requests, side-by-side
    static constexpr int kPlayersPage = 2;
    static constexpr int kHistoryPage = 3;
    static constexpr int kCloudSavesPage = 4;

    explicit NextendoAccountDialog(NextendoController* controller, Core::System& system,
                                   QWidget* parent = nullptr, int initial_page = kHomePage);
    ~NextendoAccountDialog() override;

signals:
    // Routed up to GMainWindow, which owns the one persistent chat window --
    // opens/reuses it, creates a room if needed, and sends the invite.
    void InviteToChatRequested(u64 pid, QString name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void RefreshFriends();
    void RefreshHistory();
    void RefreshPlayers();
    void OnPlayersViewClicked(const QModelIndex& index);
    void ShowPlayersContextMenu(QListView* view, const QPoint& pos);
    void ShowFriendsContextMenu(const QPoint& pos);
    void OpenReportDialog(u64 pid, const QString& name, const QString& avatar_b64);
    void SetBusy(bool busy);
    void OnAdd();

    void RunAsync(std::function<std::string()> task, std::function<void()> on_success = nullptr);

    void OnFriendsViewClicked(const QModelIndex& index);
    void OnHistoryViewClicked(const QModelIndex& index);
    u64 SelectedPid(const QModelIndex& index) const;
    void ApplyFriendFilter(const QString& text);
    void UpdateRequestsBadge(int count);
    void OnChangeAvatar();
    void OnEditUsername();
    void OnChangeBackground();
    void OnRemoveBackground();
    void ApplyBackground(const QPixmap& pixmap);
    void LoadSavedBackground();
    void UpdateDashboard();
    void UpdateHeroSizing();

    void RefreshCloudSaveTab();
    void RebuildCloudSaveTitlePicker();
    void ProbeCloudSaveAvailability(u64 title_id);

    void GoToPage(int index);
    void UpdatePageNav();
    void WireControllerNav();

    void OnDirectionalNav(int dx, int dy);
    void OnControllerActivate();
    void ActivateCurrentRow(QListView* view);
    void ActivateHistoryRow(const QModelIndex& index);
    bool ConfirmAction(const QString& title, const QString& message, const QString& yes_text,
                       const QString& no_text, const std::string& icon_base64 = {});

    NextendoController* controller;
    Core::System& system;
    Core::HID::HIDCore& hid_core;
    int initial_page = kHomePage;
    ControllerNavigation* controller_navigation = nullptr;

    QLabel* header_avatar;
    QPixmap header_avatar_source; // undecorated (square) -- re-rounded at whatever size fits
    QLabel* header_name;
    QToolButton* edit_name_button;
    QLabel* header_code;
    QLabel* status;

    QWidget* header_card = nullptr;

    QListView* friends_view;
    QStandardItemModel* friends_model;
    QStackedWidget* friends_stack;
    QListView* requests_view;
    QStandardItemModel* requests_model;
    QStackedWidget* requests_stack;
    QWidget* outgoing_requests_section;
    QLabel* outgoing_requests_label;
    QListView* outgoing_requests_view;
    QStandardItemModel* outgoing_requests_model;
    NextendoFriendDelegate* outgoing_request_delegate;
    QListView* history_view;
    QStandardItemModel* history_model;
    QStackedWidget* history_stack;
    NextendoFriendDelegate* friend_delegate;
    NextendoFriendDelegate* request_delegate;

    QLineEdit* friend_code_input;
    QPushButton* add_button;

    QLabel* lobby_state_label;
    QListView* lobby_view;
    QStandardItemModel* lobby_model;
    QStackedWidget* lobby_stack;
    NextendoFriendDelegate* lobby_delegate;
    QListView* recent_players_view;
    QStandardItemModel* recent_players_model;
    QStackedWidget* recent_players_stack;
    NextendoFriendDelegate* recent_players_delegate;
    std::unordered_set<u64> known_player_pids;

    QLineEdit* friend_search;
    QTimer refresh_timer;

    NextendoNetworkProbe* network_probe;
    QLabel* nat_label;
    QLabel* ping_label;
    QTimer* status_check_timer = nullptr;
    int status_check_dots = 0;
    bool nat_checking = false;
    bool ping_checking = false;

    QStackedWidget* pages_stack = nullptr;
    QStringList page_titles;
    int current_page = -1;
    QWidget* home_default_focus = nullptr;
    QWidget* active_popup = nullptr;
    QLabel* page_title_label = nullptr;
    NextendoPageNavArrow* nav_left_arrow = nullptr;
    NextendoPageNavArrow* nav_right_arrow = nullptr;

    QLabel* requests_badge;

    QWidget* dash_online_dot = nullptr;
    QLabel* dash_online_text = nullptr;
    QLabel* dash_friends_online_value = nullptr;
    QLabel* dash_in_game_value = nullptr;
    QWidget* dash_requests_card = nullptr;
    QLabel* dash_requests_badge = nullptr;
    QLabel* dash_requests_preview = nullptr;
    QVBoxLayout* dash_friends_list_layout = nullptr;

    QLabel* cloud_save_icon;
    QLabel* cloud_save_title;
    QLabel* cloud_save_status;
    QPushButton* cloud_save_download_button;
    class QCheckBox* cloud_save_auto_sync_checkbox;
    QWidget* cloud_save_picker_container;
    QHBoxLayout* cloud_save_picker_row;
    QButtonGroup* cloud_save_picker_group;
    u64 cloud_save_selected_title_id = 0;
    std::unordered_map<u64, bool> cloud_save_has_data;
    std::unordered_set<u64> cloud_save_probing;
};
