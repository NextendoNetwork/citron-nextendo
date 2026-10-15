// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/common_funcs.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/ns/content_management_interface.h"
#include "core/hle/service/ns/ns_types.h"
#include "core/hle/service/ns/application_manager_interface.h"

namespace Service::NS {

IContentManagementInterface::IContentManagementInterface(Core::System& system_)
    : ServiceFramework{system_, "IContentManagementInterface"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {11, D<&IContentManagementInterface::CalculateApplicationOccupiedSize>, "CalculateApplicationOccupiedSize"},
        {43, D<&IContentManagementInterface::CheckSdCardMountStatus>, "CheckSdCardMountStatus"},
        {47, D<&IContentManagementInterface::GetTotalSpaceSize>, "GetTotalSpaceSize"},
        {48, D<&IContentManagementInterface::GetFreeSpaceSize>, "GetFreeSpaceSize"},
        {71, D<&IContentManagementInterface::GetApplicationTotalSize>, "GetApplicationTotalSize"},
        {600, D<&IContentManagementInterface::CountApplicationContentMeta>, "CountApplicationContentMeta"},
        {601, D<&IContentManagementInterface::ListApplicationContentMetaStatus>, "ListApplicationContentMetaStatus"},
        {605, nullptr, "ListApplicationContentMetaStatusWithRightsCheck"},
        {607, nullptr, "IsAnyApplicationRunning"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IContentManagementInterface::~IContentManagementInterface() = default;

Result IContentManagementInterface::CountApplicationContentMeta(Out<u32> out_count,
                                                               u64 application_id) {
    R_RETURN(IApplicationManagerInterface(system).CountApplicationContentMeta(out_count,
                                                                             application_id));
}

Result IContentManagementInterface::ListApplicationContentMetaStatus(
    OutArray<ApplicationContentMetaStatus, BufferAttr_HipcMapAlias> out_entries,
    Out<s32> out_count, s32 offset, u64 application_id) {
    R_RETURN(IApplicationManagerInterface(system).ListApplicationContentMetaStatus(
        out_entries, out_count, offset, application_id));
}

Result IContentManagementInterface::CalculateApplicationOccupiedSize(
    Out<ApplicationOccupiedSize> out_size, u64 application_id) {
    *out_size = {};
    out_size->entities[0].storage_id = FileSys::StorageId::NandSystem;
    out_size->entities[1].storage_id = FileSys::StorageId::NandUser;
    out_size->entities[2].storage_id = FileSys::StorageId::SdCard;
    out_size->entities[3].storage_id = FileSys::StorageId::GameCard;
    const auto& cache = system.GetContentProviderUnion();
    const auto base_id = FileSys::GetBaseTitleID(application_id);
    for (const auto title_type : {FileSys::TitleType::Application, FileSys::TitleType::Update,
                                  FileSys::TitleType::AOC}) {
        for (const auto& [slot, entry] : cache.ListEntriesFilterOrigin(std::nullopt, title_type)) {
            if (FileSys::GetBaseTitleID(entry.title_id) != base_id) {
                continue;
            }
            const auto* provider = cache.GetSlotProvider(slot);
            const auto file = provider ? provider->GetEntryRaw(entry) : nullptr;
            if (!file) {
                continue;
            }
            const size_t index = slot == FileSys::ContentProviderUnionSlot::SysNAND ? 0
                               : slot == FileSys::ContentProviderUnionSlot::UserNAND ? 1 : 2;
            auto& entity = out_size->entities[index];
            if (title_type == FileSys::TitleType::Update) {
                entity.patch_size += file->GetSize();
            } else if (title_type == FileSys::TitleType::AOC) {
                entity.aoc_size += file->GetSize();
            } else {
                entity.app_size += file->GetSize();
            }
        }
    }
    LOG_INFO(Service_NS, "calculated content sizes, application_id={:016X}", application_id);
    R_SUCCEED();
}

Result IContentManagementInterface::CheckSdCardMountStatus() {
    LOG_WARNING(Service_NS, "(STUBBED) called");
    R_SUCCEED();
}

Result IContentManagementInterface::GetTotalSpaceSize(Out<s64> out_total_space_size,
                                                      FileSys::StorageId storage_id) {
    LOG_INFO(Service_NS, "(STUBBED) called, storage_id={}", storage_id);
    *out_total_space_size = system.GetFileSystemController().GetTotalSpaceSize(storage_id);
    R_SUCCEED();
}

Result IContentManagementInterface::GetFreeSpaceSize(Out<s64> out_free_space_size,
                                                     FileSys::StorageId storage_id) {
    LOG_INFO(Service_NS, "(STUBBED) called, storage_id={}", storage_id);
    *out_free_space_size = system.GetFileSystemController().GetFreeSpaceSize(storage_id);
    R_SUCCEED();
}

Result IContentManagementInterface::GetApplicationTotalSize(Out<s64> out_size,
                                                            Out<s64> out_free_space,
                                                            u64 application_id) {
    ApplicationOccupiedSize occupied{};
    R_TRY(CalculateApplicationOccupiedSize(&occupied, application_id));
    *out_size = 0;
    for (const auto& entity : occupied.entities) {
        *out_size += entity.app_size + entity.patch_size + entity.aoc_size;
    }
    *out_free_space = system.GetFileSystemController().GetFreeSpaceSize(FileSys::StorageId::SdCard);
    R_SUCCEED();
}

} // namespace Service::NS
