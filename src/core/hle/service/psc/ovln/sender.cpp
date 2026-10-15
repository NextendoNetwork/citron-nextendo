// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/psc/ovln/sender.h"

namespace Service::PSC {

ISender::ISender(Core::System& system_, std::shared_ptr<NotificationBus> bus_, std::string source_)
    : ServiceFramework{system_, "ISender"}, bus{std::move(bus_)}, source{std::move(source_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ISender::Send>, "Send"},
        {1, nullptr, "GetUnreceivedMessageCount"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

ISender::~ISender() = default;

Result ISender::Send(const OverlayNotification& notification) {
    LOG_INFO(Service_PSC, "Send source={} header={:016X}", source, notification[0]);
    bus->Send(source, notification, system.CoreTiming().GetClockTicks());
    R_SUCCEED();
}

} // namespace Service::PSC
