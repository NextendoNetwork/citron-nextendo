// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/bcat/news/download_context.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::News {

IDownloadContext::IDownloadContext(Core::System& system_)
    : ServiceFramework{system_, "IDownloadContext"},
      service_context{system_, "IDownloadContext"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDownloadContext::GetEvent>, "GetEvent"},
        {1, D<&IDownloadContext::Cancel>, "Cancel"},
        {2, D<&IDownloadContext::GetResult>, "GetResult"},
        {3, D<&IDownloadContext::GetSize>, "GetSize"},
        {4, D<&IDownloadContext::Unknown4>, "Unknown4"}, // [20.0.0+]
    };
    // clang-format on

    RegisterHandlers(functions);
    // No news provider is connected yet. Complete an empty result instead of
    // making HOME wait forever for an operation that was never started.
    completion_event = service_context.CreateEvent("NewsDownloadCompletion");
    completion_event->Signal();
}

IDownloadContext::~IDownloadContext() {
    service_context.CloseEvent(completion_event);
}

Result IDownloadContext::GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = &completion_event->GetReadableEvent();
    R_SUCCEED();
}

Result IDownloadContext::Cancel() {
    completion_event->Signal();
    R_SUCCEED();
}

Result IDownloadContext::GetResult() {
    LOG_INFO(Service_BCAT, "News download completed with no articles");
    R_SUCCEED();
}

Result IDownloadContext::GetSize(Out<u64> out_size) {
    *out_size = 0;
    R_SUCCEED();
}

Result IDownloadContext::Unknown4(Out<std::array<u8, 0x48>> out_progress) {
    *out_progress = {};
    R_SUCCEED();
}

} // namespace Service::News
