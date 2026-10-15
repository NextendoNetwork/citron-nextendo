// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <string>
#include "core/hle/result.h"
#include "core/hle/service/psc/ovln/notification_bus.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/psc/ovln/ovln_types.h"
#include "core/hle/service/service.h"


namespace Kernel {
class KReadableEvent;
} // namespace Kernel

namespace Service::PSC {

class IReceiver final : public ServiceFramework<IReceiver> {
public:
    explicit IReceiver(Core::System& system_, std::shared_ptr<NotificationBus> bus_);
    ~IReceiver() override;

private:
    Result AddSource(SourceName source_name);
    Result RemoveSource(SourceName source_name);
    Result GetReceiveEventHandle(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result Receive(Out<OverlayNotification> out_notification);
    Result ReceiveWithTick(Out<OverlayNotification> out_notification,
                           Out<u64> out_tick);

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* receive_event;

    std::shared_ptr<NotificationBus> bus;
    NotificationBus::Receiver receiver;
};

} // namespace Service::PSC
