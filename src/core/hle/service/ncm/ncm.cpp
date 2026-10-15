// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <array>
#include <algorithm>
#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/service/filesystem/filesystem.h"

#include "core/file_sys/romfs_factory.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/ncm/ncm.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::NCM {

class ILocationResolver final : public ServiceFramework<ILocationResolver> {
public:
    explicit ILocationResolver(Core::System& system_, FileSys::StorageId id)
        : ServiceFramework{system_, "ILocationResolver"}, storage{id} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ILocationResolver::ResolveProgramPath, "ResolveProgramPath"},
            {1, nullptr, "RedirectProgramPath"},
            {2, nullptr, "ResolveApplicationControlPath"},
            {3, nullptr, "ResolveApplicationHtmlDocumentPath"},
            {4, nullptr, "ResolveDataPath"},
            {5, nullptr, "RedirectApplicationControlPath"},
            {6, nullptr, "RedirectApplicationHtmlDocumentPath"},
            {7, nullptr, "ResolveApplicationLegalInformationPath"},
            {8, nullptr, "RedirectApplicationLegalInformationPath"},
            {9, nullptr, "Refresh"},
            {10, nullptr, "RedirectApplicationProgramPath"},
            {11, nullptr, "ClearApplicationRedirection"},
            {12, nullptr, "EraseProgramRedirection"},
            {13, nullptr, "EraseApplicationControlRedirection"},
            {14, nullptr, "EraseApplicationHtmlDocumentRedirection"},
            {15, nullptr, "EraseApplicationLegalInformationRedirection"},
            {16, nullptr, "ResolveProgramPathForDebug"},
            {17, nullptr, "RedirectProgramPathForDebug"},
            {18, nullptr, "RedirectApplicationProgramPathForDebug"},
            {19, nullptr, "EraseProgramRedirectionForDebug"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void ResolveProgramPath(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto title_id = rp.Pop<u64>();
        auto& fsc = system.GetFileSystemController();
        auto* cache = storage == FileSys::StorageId::NandSystem ? fsc.GetSystemNANDContents()
                      : storage == FileSys::StorageId::NandUser ? fsc.GetUserNANDContents()
                                                               : nullptr;
        const auto root = storage == FileSys::StorageId::NandSystem
                              ? fsc.GetSystemNANDContentDirectory()
                              : fsc.GetUserNANDContentDirectory();
        const auto file = cache ? cache->GetEntryUnparsed(title_id, FileSys::ContentRecordType::Program)
                                : nullptr;
        if (!file || !root) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(FileSys::ResultTargetNotFound);
            return;
        }
        const auto prefix = root->GetFullPath() + '/';
        const auto full_path = file->GetFullPath();
        if (!full_path.starts_with(prefix)) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(FileSys::ResultTargetNotFound);
            return;
        }
        const std::string path =
            (storage == FileSys::StorageId::NandSystem ? "@SystemContent://" : "@UserContent://") +
            full_path.substr(prefix.size());
        std::array<char, 0x301> output{};
        if (path.size() >= output.size() || ctx.GetWriteBufferSize() < output.size()) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(FileSys::ResultInvalidArgument);
            return;
        }
        std::copy(path.begin(), path.end(), output.begin());
        ctx.WriteBuffer(output);
        LOG_INFO(Service_FS, "Resolved program path: title_id={:016X} storage={}", title_id,
                 static_cast<u32>(storage));
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
    FileSys::StorageId storage;
};

class IRegisteredLocationResolver final : public ServiceFramework<IRegisteredLocationResolver> {
public:
    explicit IRegisteredLocationResolver(Core::System& system_)
        : ServiceFramework{system_, "IRegisteredLocationResolver"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "ResolveProgramPath"},
            {1, nullptr, "RegisterProgramPath"},
            {2, nullptr, "UnregisterProgramPath"},
            {3, nullptr, "RedirectProgramPath"},
            {4, nullptr, "ResolveHtmlDocumentPath"},
            {5, nullptr, "RegisterHtmlDocumentPath"},
            {6, nullptr, "UnregisterHtmlDocumentPath"},
            {7, nullptr, "RedirectHtmlDocumentPath"},
            {8, nullptr, "Refresh"},
            {9, nullptr, "RefreshExcluding"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IAddOnContentLocationResolver final : public ServiceFramework<IAddOnContentLocationResolver> {
public:
    explicit IAddOnContentLocationResolver(Core::System& system_)
        : ServiceFramework{system_, "IAddOnContentLocationResolver"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "ResolveAddOnContentPath"},
            {1, nullptr, "RegisterAddOnContentStorage"},
            {2, nullptr, "UnregisterAllAddOnContentPath"},
            {3, nullptr, "RefreshApplicationAddOnContent"},
            {4, nullptr, "UnregisterApplicationAddOnContent"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class LR final : public ServiceFramework<LR> {
public:
    explicit LR(Core::System& system_) : ServiceFramework{system_, "lr"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &LR::OpenLocationResolver, "OpenLocationResolver"},
            {1, nullptr, "OpenRegisteredLocationResolver"},
            {2, nullptr, "RefreshLocationResolver"},
            {3, nullptr, "OpenAddOnContentLocationResolver"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
private:
    void OpenLocationResolver(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto storage = rp.PopEnum<FileSys::StorageId>();
        if (storage != FileSys::StorageId::NandSystem && storage != FileSys::StorageId::NandUser) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(FileSys::ResultInvalidArgument);
            return;
        }
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ILocationResolver>(system, storage);
    }
};

class NCM final : public ServiceFramework<NCM> {
public:
    explicit NCM(Core::System& system_) : ServiceFramework{system_, "ncm"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "CreateContentStorage"},
            {1, nullptr, "CreateContentMetaDatabase"},
            {2, nullptr, "VerifyContentStorage"},
            {3, nullptr, "VerifyContentMetaDatabase"},
            {4, nullptr, "OpenContentStorage"},
            {5, nullptr, "OpenContentMetaDatabase"},
            {6, nullptr, "CloseContentStorageForcibly"},
            {7, nullptr, "CloseContentMetaDatabaseForcibly"},
            {8, nullptr, "CleanupContentMetaDatabase"},
            {9, nullptr, "ActivateContentStorage"},
            {10, nullptr, "InactivateContentStorage"},
            {11, nullptr, "ActivateContentMetaDatabase"},
            {12, nullptr, "InactivateContentMetaDatabase"},
            {13, nullptr, "InvalidateRightsIdCache"},
            {14, nullptr, "GetMemoryReport"},
            {15, nullptr, "ActivateFsContentStorage"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("lr", std::make_shared<LR>(system));
    server_manager->RegisterNamedService("ncm", std::make_shared<NCM>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::NCM
