// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"
#include "core/hle/service/kernel_helpers.h"

namespace Core {
class System;
}

namespace Kernel {
class KReadableEvent;
class KEvent;
}

namespace Service::News {

class IDownloadContext final : public ServiceFramework<IDownloadContext> {
public:
    explicit IDownloadContext(Core::System& system_);
    ~IDownloadContext() override;

private:
    Result GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result Cancel();
    Result GetResult();
    Result GetSize(Out<u64> out_size);
    Result Unknown4(Out<std::array<u8, 0x48>> out_progress); // [20.0.0+]

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completion_event{};
};

} // namespace Service::News
