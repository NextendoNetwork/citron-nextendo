// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/psc/ovln/sender.h"
#include "core/hle/service/psc/ovln/sender_service.h"

namespace Service::PSC {

ISenderService::ISenderService(Core::System& system_, std::shared_ptr<NotificationBus> bus_)
    : ServiceFramework{system_, "ovln:snd"}, bus{std::move(bus_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ISenderService::OpenSender>, "OpenSender"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

ISenderService::~ISenderService() = default;

Result ISenderService::OpenSender(Out<SharedPointer<ISender>> out_sender, SourceName source_name,
                                  std::array<u64, 2> attributes) {
    LOG_INFO(Service_PSC, "OpenSender source={} attributes={:016X} {:016X}",
             source_name.GetString(), attributes[0], attributes[1]);
    *out_sender = std::make_shared<ISender>(system, bus, source_name.GetString());
    R_SUCCEED();
}

} // namespace Service::PSC
