// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/bcat/news/news_service.h"
#include "core/hle/kernel/k_auto_object.h"
#include "core/hle/service/bcat/news/download_context.h"
#include "core/hle/service/bcat/news/newly_arrived_event_holder.h"
#include "core/hle/service/bcat/news/news_data_service.h"
#include "core/hle/service/bcat/news/news_database_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::News {

INewsService::INewsService(Core::System& system_) : ServiceFramework{system_, "INewsService"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {10100, D<&INewsService::PostLocalNews>, "PostLocalNews"},
        {20100, D<&INewsService::SetPassphrase>, "SetPassphrase"},
        {30100, D<&INewsService::GetSubscriptionStatus>, "GetSubscriptionStatus"},
        {30101, D<&INewsService::GetTopicList>, "GetTopicList"},
        {30110, D<&INewsService::Unknown30110>, "Unknown30110"},
        {30200, D<&INewsService::IsSystemUpdateRequired>, "IsSystemUpdateRequired"},
        {30201, D<&INewsService::Unknown30201>, "Unknown30201"},
        {30210, D<&INewsService::Unknown30210>, "Unknown30210"},
        {30300, D<&INewsService::RequestImmediateReception>, "RequestImmediateReception"},
        {30400, D<&INewsService::DecodeArchiveFile>, "DecodeArchiveFile"},
        {30500, D<&INewsService::Unknown30500>, "Unknown30500"},
        {30900, D<&INewsService::Unknown30900>, "Unknown30900"},
        {30901, D<&INewsService::Unknown30901>, "Unknown30901"},
        {30902, D<&INewsService::Unknown30902>, "Unknown30902"},
        {40100, D<&INewsService::SetSubscriptionStatus>, "SetSubscriptionStatus"},
        {40101, D<&INewsService::RequestAutoSubscription>, "RequestAutoSubscription"},
        {40200, D<&INewsService::ClearStorage>, "ClearStorage"},
        {40201, D<&INewsService::ClearSubscriptionStatusAll>, "ClearSubscriptionStatusAll"},
        {90100, D<&INewsService::GetNewsDatabaseDump>, "GetNewsDatabaseDump"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

INewsService::~INewsService() = default;

Result INewsService::GetSubscriptionStatus(Out<u32> out_status,
                                           InBuffer<BufferAttr_HipcPointer> buffer_data) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, buffer_size={}", buffer_data.size());
    *out_status = 0;
    R_SUCCEED();
}

Result INewsService::IsSystemUpdateRequired(Out<bool> out_is_system_update_required) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    *out_is_system_update_required = false;
    R_SUCCEED();
}

Result INewsService::RequestAutoSubscription(u64 value) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result INewsService::PostLocalNews(InBuffer<BufferAttr_HipcAutoSelect> news_data) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, news_data_size={}", news_data.size());
    R_SUCCEED();
}

Result INewsService::SetPassphrase(u64 application_id, InBuffer<BufferAttr_HipcPointer> passphrase) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, application_id={:016X}, passphrase_size={}", application_id, passphrase.size());
    R_SUCCEED();
}

Result INewsService::GetTopicList(Out<s32> out_count, OutArray<u8, BufferAttr_HipcMapAlias> out_topics) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    *out_count = 0;
    R_SUCCEED();
}

Result INewsService::Unknown30110() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result INewsService::Unknown30201() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result INewsService::Unknown30210() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result INewsService::RequestImmediateReception() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result INewsService::DecodeArchiveFile(InBuffer<BufferAttr_HipcPointer> archive_data, OutBuffer<BufferAttr_HipcMapAlias> out_decoded_data) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, archive_size={}", archive_data.size());
    R_SUCCEED();
}

Result INewsService::Unknown30500(OutInterface<IDownloadContext> out_context,
                                 InBuffer<BufferAttr_HipcPointer> filter,
                                 InCopyHandle<Kernel::KAutoObject> transfer_handle,
                                 u64 transfer_size) {
    // Firmware's INewsService returns an IDownloadContext domain object.
    // Returning success without it makes the client use an invalid object id.
    LOG_INFO(Service_BCAT, "Create download context: filter_bytes={} transfer_size={} handle={}",
             filter.size(), transfer_size, transfer_handle.Get() != nullptr);
    *out_context = std::make_shared<IDownloadContext>(system);
    R_SUCCEED();
}

Result INewsService::Unknown30900(OutInterface<INewlyArrivedEventHolder> out_interface) {
    // Legacy (1.0.0) INewsService factory: success must include the domain object.
    LOG_INFO(Service_BCAT, "called");
    *out_interface = std::make_shared<INewlyArrivedEventHolder>(system);
    R_SUCCEED();
}

Result INewsService::Unknown30901(OutInterface<INewsDataService> out_interface) {
    // Legacy (1.0.0) INewsService factory: success must include the domain object.
    LOG_INFO(Service_BCAT, "called");
    *out_interface = std::make_shared<INewsDataService>(system);
    R_SUCCEED();
}

Result INewsService::Unknown30902(OutInterface<INewsDatabaseService> out_interface) {
    // Legacy (1.0.0) INewsService factory: success must include the domain object.
    LOG_INFO(Service_BCAT, "called");
    *out_interface = std::make_shared<INewsDatabaseService>(system);
    R_SUCCEED();
}

Result INewsService::SetSubscriptionStatus(InBuffer<BufferAttr_HipcPointer> subscription_data) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, subscription_data_size={}", subscription_data.size());
    R_SUCCEED();
}

Result INewsService::ClearStorage() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result INewsService::ClearSubscriptionStatusAll() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result INewsService::GetNewsDatabaseDump(OutBuffer<BufferAttr_HipcMapAlias> out_database_dump) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

} // namespace Service::News
