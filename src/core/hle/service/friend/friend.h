// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/service.h"

namespace Core {
class System;
}
namespace Kernel {
class KEvent;
}

namespace Service::Friend {

class Module final {
public:
    Kernel::KEvent* invitation_deferral_event{};
    class Interface : public ServiceFramework<Interface> {
    public:
        explicit Interface(std::shared_ptr<Module> module_, Core::System& system_,
                           const char* name);
        ~Interface() override;

        void CreateFriendService(HLERequestContext& ctx);
        void CreateNotificationService(HLERequestContext& ctx);
        void CreateDaemonSuspendSessionService(HLERequestContext& ctx);

    protected:
        std::shared_ptr<Module> module;
    };
};

void LoopProcess(Core::System& system);

// [Nextendo] Signals every live INotificationService that the friends list has (potentially)
// changed, so it re-fetches and re-renders. Real hardware pushes this whenever presence/friend
// data actually updates; citron's INotificationService only ever signaled it once, at
// construction time -- before the background poll had a chance to populate real data -- so
// nothing ever told a viewer already on-screen to check again. Safe to call from any thread.
void NotifyFriendsListUpdated();

} // namespace Service::Friend
