// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include "citron/nextendo_game_invites.h"
#include "common/nextendo_account.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/nca_metadata.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"

namespace {

s64 Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool Expired(const WebService::NextendoApi::GameInvitation& invite) {
    return invite.expires_at != 0 && invite.expires_at <= Now();
}

struct Fetched {
    WebService::NextendoApi::GameInvitationList list;
    std::map<u64, std::string> avatars;
};

} // Anonymous namespace

NextendoGameInvites::NextendoGameInvites(Core::System& system_, QObject* parent)
    : QObject(parent), system{system_} {
    poll_timer.setInterval(10000);
    // Outside a game only the Friends page asks, so idle clients don't poll.
    connect(&poll_timer, &QTimer::timeout, this, [this] {
        if (!launching_invitation.isEmpty()) {
            if (!system.IsPoweredOn() || launching_generation != Common::NextendoAccount::GetGeneration()) {
                launching_invitation.clear();
            } else {
                auto* windows = system.GetAppletManager().GetWindowSystem();
                const auto app = windows ? const_cast<Service::AM::WindowSystem*>(windows)->GetMainApplet() : nullptr;
                const auto pending = std::find_if(invites.begin(), invites.end(), [this](const Invite& invite) {
                    return invite.id == launching_invitation.toStdString();
                });
                if (app && pending != invites.end() && app->program_id == pending->title_id) {
                    Decline(launching_invitation);
                    launching_invitation.clear();
                }
            }
        }
        if (system.IsPoweredOn()) {
            Refresh();
        } else if (PruneExpired()) {
            emit Changed();
        }
    });
    poll_timer.start();
}

std::vector<NextendoGameInvites::Invite> NextendoGameInvites::Pending() const {
    std::vector<Invite> out;
    std::copy_if(invites.begin(), invites.end(), std::back_inserter(out),
                 [](const Invite& invite) { return !Expired(invite); });
    return out;
}

QString NextendoGameInvites::Avatar(u64 pid) const {
    const auto it = avatars.find(pid);
    return it == avatars.end() ? QString{} : QString::fromStdString(it->second);
}

void NextendoGameInvites::Refresh() {
    if (fetching || !Common::NextendoAccount::IsLinked()) return;
    fetching = true;
    const auto gen = Common::NextendoAccount::GetGeneration();
    std::set<u64> known;
    for (const auto& [pid, avatar] : avatars) known.insert(pid);
    auto* task = new QFutureWatcher<Fetched>(this);
    connect(task, &QFutureWatcher<Fetched>::finished, this, [this, task, gen] {
        fetching = false;
        auto fetched = task->result();
        task->deleteLater();
        if (!fetched.list.ok || gen != Common::NextendoAccount::GetGeneration()) return;
        if (gen != generation) {
            generation = gen;
            seen.clear();
            avatars.clear();
        }
        avatars.merge(fetched.avatars);
        invites = std::move(fetched.list.invitations);
        std::erase_if(invites, Expired);
        std::sort(invites.begin(), invites.end(),
                  [](const Invite& a, const Invite& b) { return a.expires_at > b.expires_at; });
        std::set<std::string> current;
        for (const auto& invite : invites) {
            current.insert(invite.id);
            // HOME must receive invitations too, before the invited game starts.
            if (!seen.contains(invite.id)) {
                LOG_INFO(Frontend, "Nextendo invitation received: title={:016X}", invite.title_id);
                emit Received(QString::fromStdString(invite.id),
                              QString::fromStdString(invite.sender_name), invite.sender_pid);
            }
        }
        seen = std::move(current);
        emit Changed();
    });
    task->setFuture(QtConcurrent::run([known] {
        Fetched out{WebService::NextendoApi::GetGameInvitations(), {}};
        for (const auto& invite : out.list.invitations) {
            if (!known.contains(invite.sender_pid) && !out.avatars.contains(invite.sender_pid)) {
                out.avatars[invite.sender_pid] =
                    WebService::NextendoApi::GetAvatarByPid(invite.sender_pid);
            }
        }
        return out;
    }));
}

QString NextendoGameInvites::Accept(const QString& id) {
    const auto it = std::find_if(invites.begin(), invites.end(), [&](const Invite& invite) {
        return invite.id == id.toStdString();
    });
    if (it == invites.end() || Expired(*it)) {
        Remove(id);
        return tr("This invitation has expired.");
    }
    const auto* windows =
        system.IsPoweredOn() ? system.GetAppletManager().GetWindowSystem() : nullptr;
    const auto app =
        windows ? const_cast<Service::AM::WindowSystem*>(windows)->GetMainApplet() : nullptr;
    if (!app) return tr("Open HOME before accepting this invitation.");
    const auto user =
        system.GetProfileManager().GetUser(Settings::values.current_user.GetValue());
    if (!user) {
        return tr("Select a user profile first.");
    }
    std::vector<u8> storage(sizeof(*user) + it->user_data.size());
    std::memcpy(storage.data(), &*user, sizeof(*user));
    std::copy(it->user_data.begin(), it->user_data.end(), storage.begin() + sizeof(*user));
    if (app->program_id != it->title_id) {
        if (app->applet_id != Service::AM::AppletId::QLaunch) {
            return tr("Close the current game from HOME before accepting this invitation.");
        }
        if (!system.GetContentProviderUnion().GetEntryRaw(it->title_id, FileSys::ContentRecordType::Program)) {
            return tr("Install the invited game before accepting this invitation.");
        }
        if (!const_cast<Service::AM::WindowSystem*>(windows)->RequestInvitationLaunch(it->title_id, std::move(storage))) {
            return tr("HOME already has a pending game launch.");
        }
        launching_invitation = id;
        launching_generation = Common::NextendoAccount::GetGeneration();
        LOG_INFO(Frontend, "Accepted invitation queued for HOME: title={:016X}", it->title_id);
        return {};
    }
    {
        std::scoped_lock lock{app->lock};
        if (app->friend_invitation_storage_channel.size() >= 32) {
            return tr("The game isn't accepting invitations right now.");
        }
        app->friend_invitation_storage_channel.push_back(std::move(storage));
        app->friend_invitation_storage_channel_event.Signal();
    }
    const_cast<Service::AM::WindowSystem*>(windows)->RequestApplicationToGetForeground();
    Decline(id);
    return {};
}

void NextendoGameInvites::Decline(const QString& id) {
    (void)QtConcurrent::run([invitation_id = id.toStdString()] {
        return WebService::NextendoApi::DismissGameInvitation(invitation_id);
    });
    Remove(id);
}

bool NextendoGameInvites::PruneExpired() {
    return std::erase_if(invites, Expired) > 0;
}

void NextendoGameInvites::Remove(const QString& id) {
    if (std::erase_if(invites, [&](const Invite& invite) { return invite.id == id.toStdString(); })) {
        emit Changed();
    }
}
