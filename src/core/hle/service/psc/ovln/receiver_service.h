// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Service::PSC {

class IReceiver;
class NotificationBus;

class IReceiverService final : public ServiceFramework<IReceiverService> {
public:
    explicit IReceiverService(Core::System& system_, std::shared_ptr<NotificationBus> bus_);
    ~IReceiverService() override;

private:
    std::shared_ptr<NotificationBus> bus;
    Result OpenReceiver(Out<SharedPointer<IReceiver>> out_receiver);
};

} // namespace Service::PSC
