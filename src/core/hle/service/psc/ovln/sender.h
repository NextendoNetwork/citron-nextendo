// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/psc/ovln/notification_bus.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/psc/ovln/ovln_types.h"
#include "core/hle/service/service.h"

namespace Service::PSC {

class ISender final : public ServiceFramework<ISender> {
public:
    explicit ISender(Core::System& system_, std::shared_ptr<NotificationBus> bus_, std::string source_);
    ~ISender() override;

private:
    Result Send(const OverlayNotification& notification);
    std::shared_ptr<NotificationBus> bus;
    std::string source;
};

} // namespace Service::PSC
