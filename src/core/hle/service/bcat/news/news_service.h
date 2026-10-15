// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KAutoObject;
}

namespace Service::News {

class IDownloadContext;
class INewlyArrivedEventHolder;
class INewsDataService;
class INewsDatabaseService;

class INewsService final : public ServiceFramework<INewsService> {
public:
    explicit INewsService(Core::System& system_);
    ~INewsService() override;

private:
    Result GetSubscriptionStatus(Out<u32> out_status, InBuffer<BufferAttr_HipcPointer> buffer_data);

    Result IsSystemUpdateRequired(Out<bool> out_is_system_update_required);

    Result RequestAutoSubscription(u64 value);

    // Additional News service functions
    Result PostLocalNews(InBuffer<BufferAttr_HipcAutoSelect> news_data);
    Result SetPassphrase(u64 application_id, InBuffer<BufferAttr_HipcPointer> passphrase);
    Result GetTopicList(Out<s32> out_count, OutArray<u8, BufferAttr_HipcMapAlias> out_topics);
    Result Unknown30110();
    Result Unknown30201();
    Result Unknown30210();
    Result RequestImmediateReception();
    Result DecodeArchiveFile(InBuffer<BufferAttr_HipcPointer> archive_data, OutBuffer<BufferAttr_HipcMapAlias> out_decoded_data);
    Result Unknown30500(OutInterface<IDownloadContext> out_context,
                        InBuffer<BufferAttr_HipcPointer> filter,
                        InCopyHandle<Kernel::KAutoObject> transfer_handle, u64 transfer_size);
    Result Unknown30900(OutInterface<INewlyArrivedEventHolder> out_interface);
    Result Unknown30901(OutInterface<INewsDataService> out_interface);
    Result Unknown30902(OutInterface<INewsDatabaseService> out_interface);
    Result SetSubscriptionStatus(InBuffer<BufferAttr_HipcPointer> subscription_data);
    Result ClearStorage();
    Result ClearSubscriptionStatusAll();
    Result GetNewsDatabaseDump(OutBuffer<BufferAttr_HipcMapAlias> out_database_dump);
};

} // namespace Service::News
