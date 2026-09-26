// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <queue>
#include <fmt/format.h>
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/acc/errors.h"
#include "common/nextendo_account.h"
#include "common/nextendo_friends.h"
#include "common/nextendo_nat.h"
#include "core/hle/service/friend/friend.h"
#include "core/hle/service/friend/friend_interface.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/server_manager.h"

// [UNITY-FIX] undef Win32 macros shadowing ServiceContext methods.
#undef CreateEvent
#undef CreateMutex
#undef CreateSemaphore

namespace Service::Friend {

namespace {

#pragma pack(push, 1)
struct UserPresenceImpl {
    Common::UUID user_id;                  // 0x00
    s64 last_time_online_timestamp;        // 0x10
    u32 status;                            // 0x18
    u8 same_presence_group_application;    // 0x1C
    std::array<u8, 3> unknown;             // 0x1D
    std::array<u8, 0xC0> app_key_value;    // 0x20
};
static_assert(sizeof(UserPresenceImpl) == 0xE0, "UserPresenceImpl has the wrong size");

struct FriendImpl {
    Common::UUID user_id;         // 0x00
    u64 network_user_id;          // 0x10
    std::array<char, 0x21> nickname; // 0x18
    std::array<u8, 7> nickname_pad;  // 0x39, aligns presence to 0x40
    UserPresenceImpl presence;    // 0x40
    u8 is_favourite;              // 0x120
    u8 is_new;                    // 0x121
    std::array<u8, 6> unknown;    // 0x122
    u8 is_valid;                  // 0x128
    std::array<u8, 0xD7> padding; // to 0x200
};
static_assert(sizeof(FriendImpl) == 0x200, "FriendImpl has the wrong size");
static_assert(offsetof(FriendImpl, presence) == 0x40);
static_assert(offsetof(FriendImpl, is_favourite) == 0x120);
static_assert(offsetof(FriendImpl, is_valid) == 0x128);
#pragma pack(pop)

// [Nextendo] InGame never leaves "0" for a hosted private battle even once NAT resolution is
// confirmed (nextendo_nat_rewrite.h fixes the station data itself). Same-length in-place fix.
bool FixupInGameFlag(std::string& app_field) {
    if (!Common::NextendoNat::GetObservedExternalIp()) {
        return false;
    }

    struct Token {
        size_t offset;
        std::string text;
    };
    std::vector<Token> tokens;
    size_t pos = 0;
    while (pos < app_field.size()) {
        const size_t end = app_field.find('\0', pos);
        const size_t token_end = end == std::string::npos ? app_field.size() : end;
        if (token_end == pos) {
            break; // an empty token marks the start of the zero-padded tail
        }
        tokens.push_back({pos, app_field.substr(pos, token_end - pos)});
        pos = token_end + 1;
    }

    std::optional<std::string> mode;
    std::optional<size_t> in_game_value_offset;
    std::optional<std::string> in_game_value;
    for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
        if (tokens[i].text == "Mode") {
            mode = tokens[i + 1].text;
        } else if (tokens[i].text == "InGame") {
            in_game_value_offset = tokens[i + 1].offset;
            in_game_value = tokens[i + 1].text;
        }
    }

    if (mode != "cPrivate" || !in_game_value_offset || in_game_value != "0") {
        return false;
    }

    app_field[*in_game_value_offset] = '1';
    return true;
}

// [Nextendo] ARMS reports raw presence status 1 (Online) even while actively hosting/queued
// for a match, so friends never see it as OnlinePlay and the Friends List UI buckets them as
// "online, nothing joinable" without ever reading JoinMode at all (confirmed by decompiling
// the ARMS update binary: the friends-list categorization only inspects JoinMode when the
// caller's presence status is already 2). ARMS's app_field uses the same null-delimited
// key/value scheme as Splatoon 2's, just different keys (SessionId/JoinMode/HasPassword
// instead of Mode/InGame). JoinMode is 0 only when there is no active session; any of 1-4
// means a real session exists, so bump the status the same way FixupInGameFlag does for
// Splatoon 2's InGame flag.
bool IsArmsSessionActive(const std::string& app_field) {
    size_t pos = 0;
    while (pos < app_field.size()) {
        const size_t end = app_field.find('\0', pos);
        const size_t token_end = end == std::string::npos ? app_field.size() : end;
        if (token_end == pos) {
            break;
        }
        const std::string key = app_field.substr(pos, token_end - pos);
        pos = token_end + 1;
        if (pos >= app_field.size()) {
            break;
        }
        const size_t value_end = app_field.find('\0', pos);
        const size_t value_token_end = value_end == std::string::npos ? app_field.size() : value_end;
        const std::string value = app_field.substr(pos, value_token_end - pos);
        pos = value_token_end + 1;

        if (key == "JoinMode") {
            return value == "1" || value == "2" || value == "3" || value == "4";
        }
    }
    return false;
}

// The account server hands out NEX PIDs; the guest wants a Uid. Derive one deterministically so a
// friend keeps the same Uid across launches. The high half matches what Ryujinx uses.
Common::UUID UidForPid(u64 pid) {
    std::array<u8, 16> raw{};
    std::memcpy(raw.data(), &pid, sizeof(pid));
    const u64 high = 0x1100000000000000ULL;
    std::memcpy(raw.data() + 8, &high, sizeof(high));
    return Common::UUID{raw};
}

#pragma pack(push, 1)
// Offsets as nnSdk's Profile accessors read them.
struct ProfileImpl {
    u64 network_user_id;              // 0x00
    std::array<char, 0x21> nickname;  // 0x08
    std::array<u8, 7> nickname_pad;   // 0x29
    std::array<char, 0xA0> image_url; // 0x30
    u8 is_valid;                      // 0xD0
    std::array<u8, 0x2F> reserved;    // 0xD1
};
static_assert(sizeof(ProfileImpl) == 0x100, "ProfileImpl has the wrong size");
static_assert(offsetof(ProfileImpl, nickname) == 0x08);
static_assert(offsetof(ProfileImpl, image_url) == 0x30);
static_assert(offsetof(ProfileImpl, is_valid) == 0xD0);
#pragma pack(pop)

// Titles may ask by Nextendo pid or by BAAS account id.
bool FriendMatchesId(const Common::NextendoFriends::Entry& entry, u64 id) {
    return entry.pid == id || (entry.account_id != 0 && entry.account_id == id);
}

// List a friend under the id their own client announces: account id on hardware, pid on emulators.
u64 FriendAdvertisedId(const Common::NextendoFriends::Entry& entry) {
    return entry.is_console && entry.account_id != 0 ? entry.account_id : entry.pid;
}

// [Nextendo] Resolves a pid to a display name for GetProfileList/GetProfileExtraList: the local
// account itself (via NextendoAccount, not covered by the friends cache) or an actual Nextendo
// friend. Strangers (e.g. a balloon owner you're not friends with) aren't resolvable here --
// titles that also carry a plain name string on the wire (Odyssey's DataStoreSearchBalloonResult.
// ownerName) fall back to that; this only covers the profile-lookup path.
bool ResolveProfileName(u64 pid, std::string& out_name) {
    if (Common::NextendoAccount::IsLinked() && Common::NextendoAccount::GetPid() == pid) {
        out_name = Common::NextendoAccount::GetUsername();
        return true;
    }
    const auto entries = Common::NextendoFriends::Get();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                  [pid](const auto& e) { return FriendMatchesId(e, pid); });
    if (it != entries.end()) {
        out_name = it->name;
        return true;
    }
    return false;
}

std::optional<ProfileImpl> MakeProfile(u64 pid) {
    std::string name;
    if (!ResolveProfileName(pid, name)) {
        return std::nullopt;
    }
    ProfileImpl out{};
    out.network_user_id = pid;
    const auto length = std::min(name.size(), out.nickname.size() - 1);
    std::memcpy(out.nickname.data(), name.data(), length);

    // Same avatar URL a console is given for this friend; a title rendering the profile fetches it.
    u64 account_id = 0;
    const auto entries = Common::NextendoFriends::Get();
    if (const auto it = std::find_if(entries.begin(), entries.end(),
                                     [pid](const auto& e) { return FriendMatchesId(e, pid); });
        it != entries.end()) {
        account_id = it->account_id;
    }
    const std::string url =
        account_id != 0
            ? fmt::format("https://cdn-image-e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com/1/fr_{:016x}",
                          account_id)
            : std::string{"https://cdn-image-e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com/1/default"};
    std::memcpy(out.image_url.data(), url.data(), std::min(url.size(), out.image_url.size() - 1));

    out.is_valid = 1;
    return out;
}

// Records carry the id the caller asked about, or the title cannot pair them with its query.
FriendImpl MakeFriend(const Common::NextendoFriends::Entry& entry, u64 requested_id = 0,
                       const Common::UUID* owner = nullptr) {
    FriendImpl out{};
    const u64 advertised = requested_id != 0 ? requested_id : FriendAdvertisedId(entry);
    // nnSdk's Friend::GetProfileImage checks this Uid is a local user before it asks the service.
    out.user_id = owner != nullptr ? *owner : UidForPid(advertised);
    out.network_user_id = advertised;

    const auto length = std::min(entry.name.size(), out.nickname.size() - 1);
    std::memcpy(out.nickname.data(), entry.name.data(), length);

    out.presence.user_id = UidForPid(advertised);
    out.presence.status = static_cast<u32>(entry.status);
    out.presence.last_time_online_timestamp = 0x7FFFFFFFFFFFFFFFLL;
    // In this application only while online; an offline in-app friend has no session to find.
    out.presence.same_presence_group_application = entry.status > 0 ? 1 : 0;

    const auto blob = std::min(entry.app_field.size(), out.presence.app_key_value.size());
    std::memcpy(out.presence.app_key_value.data(), entry.app_field.data(), blob);

    out.is_valid = 1;
    return out;
}

} // Anonymous namespace


class IFriendService final : public ServiceFramework<IFriendService> {
public:
    explicit IFriendService(Core::System& system_)
        : ServiceFramework{system_, "IFriendService"}, service_context{system, "IFriendService"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IFriendService::GetCompletionEvent, "GetCompletionEvent"},
            {1, &IFriendService::Cancel, "Cancel"},
            {10100, &IFriendService::GetFriendListIds, "GetFriendListIds"},
            {10101, &IFriendService::GetFriendList, "GetFriendList"},
            {10102, &IFriendService::UpdateFriendInfo, "UpdateFriendInfo"},
            {10110, &IFriendService::GetFriendProfileImage, "GetFriendProfileImage"},
            {10111, &IFriendService::GetFriendProfileImageWithImageSize, "GetFriendProfileImageWithImageSize"},
            {10120, &IFriendService::CheckFriendListAvailability, "CheckFriendListAvailability"},
            {10121, &IFriendService::EnsureFriendListAvailable, "EnsureFriendListAvailable"},
            {10200, &IFriendService::SendFriendRequestForApplication, "SendFriendRequestForApplication"},
            {10211, &IFriendService::AddFacedFriendRequestForApplication, "AddFacedFriendRequestForApplication"},
            {10400, &IFriendService::GetBlockedUserListIds, "GetBlockedUserListIds"},
            {10420, &IFriendService::CheckBlockedUserListAvailability, "CheckBlockedUserListAvailability"},
            {10421, &IFriendService::EnsureBlockedUserListAvailable, "EnsureBlockedUserListAvailable"},
            {10500, &IFriendService::GetProfileList, "GetProfileList"},
            {10501, &IFriendService::GetProfileListV2, "GetProfileListV2"},
            {10600, &IFriendService::DeclareOpenOnlinePlaySession, "DeclareOpenOnlinePlaySession"},
            {10601, &IFriendService::DeclareCloseOnlinePlaySession, "DeclareCloseOnlinePlaySession"},
            {10610, &IFriendService::UpdateUserPresence, "UpdateUserPresence"},
            {10700, &IFriendService::GetPlayHistoryRegistrationKey, "GetPlayHistoryRegistrationKey"},
            {10701, &IFriendService::GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId"},
            {10702, &IFriendService::AddPlayHistory, "AddPlayHistory"},
            {11000, &IFriendService::GetProfileImageUrl, "GetProfileImageUrl"},
            {11001, &IFriendService::GetProfileImageUrlV2, "GetProfileImageUrlV2"},
            {20100, &IFriendService::GetFriendCount, "GetFriendCount"},
            {20101, &IFriendService::GetNewlyFriendCount, "GetNewlyFriendCount"},
            {20102, &IFriendService::GetFriendDetailedInfo, "GetFriendDetailedInfo"},
            {20103, &IFriendService::SyncFriendList, "SyncFriendList"},
            {20104, &IFriendService::RequestSyncFriendList, "RequestSyncFriendList"},
            {20105, &IFriendService::GetFriendListForViewer, "GetFriendListForViewer"},
            {20106, &IFriendService::UpdateFriendInfoForViewer, "UpdateFriendInfoForViewer"},
            {20107, &IFriendService::GetFriendDetailedInfoV2, "GetFriendDetailedInfoV2"},
            {20110, &IFriendService::LoadFriendSetting, "LoadFriendSetting"},
            {20200, &IFriendService::GetReceivedFriendRequestCount, "GetReceivedFriendRequestCount"},
            {20201, &IFriendService::GetFriendRequestList, "GetFriendRequestList"},
            {20202, &IFriendService::GetFriendRequestListV2, "GetFriendRequestListV2"},
            {20300, &IFriendService::GetFriendCandidateList, "GetFriendCandidateList"},
            {20301, &IFriendService::GetNintendoNetworkIdInfo, "GetNintendoNetworkIdInfo"},
            {20302, &IFriendService::GetSnsAccountLinkage, "GetSnsAccountLinkage"},
            {20303, &IFriendService::GetSnsAccountProfile, "GetSnsAccountProfile"},
            {20304, &IFriendService::GetSnsAccountFriendList, "GetSnsAccountFriendList"},
            {20400, &IFriendService::GetBlockedUserList, "GetBlockedUserList"},
            {20401, &IFriendService::SyncBlockedUserList, "SyncBlockedUserList"},
            {20402, &IFriendService::GetBlockedUserListV2, "GetBlockedUserListV2"},
            {20500, &IFriendService::GetProfileExtraList, "GetProfileExtraList"},
            {20501, &IFriendService::GetRelationship, "GetRelationship"},
            {20502, &IFriendService::GetProfileExtraListV2, "GetProfileExtraListV2"},
            {20600, &IFriendService::GetUserPresenceView, "GetUserPresenceView"},
            {20601, &IFriendService::GetUserPresenceViewV2, "GetUserPresenceViewV2"},
            {20700, &IFriendService::GetPlayHistoryList, "GetPlayHistoryList"},
            {20701, &IFriendService::GetPlayHistoryStatistics, "GetPlayHistoryStatistics"},
            {20702, &IFriendService::GetPlayHistoryListV2, "GetPlayHistoryListV2"},
            {20800, &IFriendService::LoadUserSetting, "LoadUserSetting"},
            {20801, &IFriendService::SyncUserSetting, "SyncUserSetting"},
            {20802, &IFriendService::LoadUserSettingV2, "LoadUserSettingV2"},
            {20900, &IFriendService::RequestListSummaryOverlayNotification, "RequestListSummaryOverlayNotification"},
            {21000, &IFriendService::GetExternalApplicationCatalog, "GetExternalApplicationCatalog"},
            {22000, &IFriendService::GetReceivedFriendInvitationList, "GetReceivedFriendInvitationList"},
            {22001, &IFriendService::GetReceivedFriendInvitationDetailedInfo, "GetReceivedFriendInvitationDetailedInfo"},
            {22002, &IFriendService::GetReceivedFriendInvitationListV2, "GetReceivedFriendInvitationListV2"},
            {22003, &IFriendService::GetReceivedFriendInvitationDetailedInfoV2, "GetReceivedFriendInvitationDetailedInfoV2"},
            {22010, &IFriendService::GetReceivedFriendInvitationCountCache, "GetReceivedFriendInvitationCountCache"},
            {30100, &IFriendService::DropFriendNewlyFlags, "DropFriendNewlyFlags"},
            {30101, &IFriendService::DeleteFriend, "DeleteFriend"},
            {30110, &IFriendService::DropFriendNewlyFlag, "DropFriendNewlyFlag"},
            {30120, &IFriendService::ChangeFriendFavoriteFlag, "ChangeFriendFavoriteFlag"},
            {30121, &IFriendService::ChangeFriendOnlineNotificationFlag, "ChangeFriendOnlineNotificationFlag"},
            {30200, &IFriendService::SendFriendRequest, "SendFriendRequest"},
            {30201, &IFriendService::SendFriendRequestWithApplicationInfo, "SendFriendRequestWithApplicationInfo"},
            {30202, &IFriendService::CancelFriendRequest, "CancelFriendRequest"},
            {30203, &IFriendService::AcceptFriendRequest, "AcceptFriendRequest"},
            {30204, &IFriendService::RejectFriendRequest, "RejectFriendRequest"},
            {30205, &IFriendService::ReadFriendRequest, "ReadFriendRequest"},
            {30210, &IFriendService::GetFacedFriendRequestRegistrationKey, "GetFacedFriendRequestRegistrationKey"},
            {30211, &IFriendService::AddFacedFriendRequest, "AddFacedFriendRequest"},
            {30212, &IFriendService::CancelFacedFriendRequest, "CancelFacedFriendRequest"},
            {30213, &IFriendService::GetFacedFriendRequestProfileImage, "GetFacedFriendRequestProfileImage"},
            {30214, &IFriendService::GetFacedFriendRequestProfileImageFromPath, "GetFacedFriendRequestProfileImageFromPath"},
            {30215, &IFriendService::SendFriendRequestWithExternalApplicationCatalogId, "SendFriendRequestWithExternalApplicationCatalogId"},
            {30216, &IFriendService::ResendFacedFriendRequest, "ResendFacedFriendRequest"},
            {30217, &IFriendService::SendFriendRequestWithNintendoNetworkIdInfo, "SendFriendRequestWithNintendoNetworkIdInfo"},
            {30218, &IFriendService::SendFriendRequestWithApplicationInfoV2, "SendFriendRequestWithApplicationInfoV2"},
            {30300, &IFriendService::GetSnsAccountLinkPageUrl, "GetSnsAccountLinkPageUrl"},
            {30301, &IFriendService::UnlinkSnsAccount, "UnlinkSnsAccount"},
            {30400, &IFriendService::BlockUser, "BlockUser"},
            {30401, &IFriendService::BlockUserWithApplicationInfo, "BlockUserWithApplicationInfo"},
            {30402, &IFriendService::UnblockUser, "UnblockUser"},
            {30403, &IFriendService::BlockUserWithApplicationInfoV2, "BlockUserWithApplicationInfoV2"},
            {30500, &IFriendService::GetProfileExtraFromFriendCode, "GetProfileExtraFromFriendCode"},
            {30501, &IFriendService::GetProfileExtraFromFriendCodeV2, "GetProfileExtraFromFriendCodeV2"},
            {30700, &IFriendService::DeletePlayHistory, "DeletePlayHistory"},
            {30701, &IFriendService::AddPlayHistoryWithApplication, "AddPlayHistoryWithApplication"},
            {30810, &IFriendService::ChangePresencePermission, "ChangePresencePermission"},
            {30811, &IFriendService::ChangeFriendRequestReception, "ChangeFriendRequestReception"},
            {30812, &IFriendService::ChangePlayLogPermission, "ChangePlayLogPermission"},
            {30820, &IFriendService::IssueFriendCode, "IssueFriendCode"},
            {30830, &IFriendService::ClearPlayLog, "ClearPlayLog"},
            {30900, &IFriendService::SendFriendInvitation, "SendFriendInvitation"},
            {30901, &IFriendService::SendFriendInvitationV2, "SendFriendInvitationV2"},
            {30910, &IFriendService::ReadFriendInvitation, "ReadFriendInvitation"},
            {30911, &IFriendService::ReadAllFriendInvitations, "ReadAllFriendInvitations"},
            {31000, &IFriendService::OpenUser, "OpenUser"},
            {40100, &IFriendService::DeleteFriendListCache, "DeleteFriendListCache"},
            {40400, &IFriendService::DeleteBlockedUserListCache, "DeleteBlockedUserListCache"},
            {49900, &IFriendService::DeleteNetworkServiceAccountCache, "DeleteNetworkServiceAccountCache"},
        };
        // clang-format on

        RegisterHandlers(functions);

        completion_event = service_context.CreateEvent("IFriendService:CompletionEvent");
    }

    ~IFriendService() override {
        service_context.CloseEvent(completion_event);
    }

    // Real hardware answers most of these commands asynchronously and signals the shared
    // completion event once the underlying (often network-backed) work finishes; the caller
    // waits on that event before treating the data as ready. citron's stubs finish instantly,
    // but individually forgetting to signal completion_event in any one of them leaves the
    // guest polling forever, so signal it centrally after every command in this class instead.
    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& context) override {
        const Result result = ServiceFrameworkBase::HandleSyncRequest(session, context);
        completion_event->Signal();
        return result;
    }

    void GetCompletionEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "GetCompletionEvent called");
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(completion_event->GetReadableEvent());
    }

    void GetFriendListIds(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto offset = rp.Pop<s32>();

        u32 count = 0;
        if (offset == 0 && ctx.CanWriteBuffer()) {
            // [Nextendo] Splatoon 3 asks for its friend list exactly once, early in boot, and
            // never asks again -- unlike NEX titles, which poll this repeatedly from their own
            // online thread (where blocking would stall PRUDP acks and the server would declare
            // a communication error, so they keep the plain non-blocking Get()). If the
            // frontend's background refresh hasn't populated the cache yet by the time Splatoon
            // 3's one-shot request lands, it's stuck with zero friends for the whole session.
            // Give that one call a short, bounded wait instead. Matches Ryujinx-Nextendo's own
            // fix here (NextendoFriends.GetWarm), which measured this exact race.
            constexpr u64 SplatoonThreeTitleId = 0x0100C2500FC20000ULL;
            const auto entries = system.GetApplicationProcessProgramID() == SplatoonThreeTitleId
                                     ? Common::NextendoFriends::GetWarm(2000)
                                     : Common::NextendoFriends::Get();
            const auto capacity = ctx.GetWriteBufferNumElements<u64>();
            std::vector<u64> ids;
            for (const auto& entry : entries) {
                if (ids.size() >= capacity) {
                    break;
                }
                ids.push_back(FriendAdvertisedId(entry));
            }
            if (!ids.empty()) {
                ctx.WriteBuffer(ids);
            }
            count = static_cast<u32>(ids.size());
        }

        LOG_INFO(Service_Friend, "[Nextendo] GetFriendListIds -> {}", count);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(count);
    }

    void GetReceivedFriendInvitationCountCache(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "(STUBBED) GetReceivedFriendInvitationCountCache called");
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(0);
    }

private:
    void GetFriendList(HLERequestContext& ctx);
    void CheckFriendListAvailability(HLERequestContext& ctx);
    void GetBlockedUserListIds(HLERequestContext& ctx);
    void CheckBlockedUserListAvailability(HLERequestContext& ctx);
    void DeclareCloseOnlinePlaySession(HLERequestContext& ctx);
    void UpdateUserPresence(HLERequestContext& ctx);
    void GetPlayHistoryRegistrationKey(HLERequestContext& ctx);
    void GetFriendCount(HLERequestContext& ctx);
    void GetNewlyFriendCount(HLERequestContext& ctx);
    void GetReceivedFriendRequestCount(HLERequestContext& ctx);
    void GetPlayHistoryStatistics(HLERequestContext& ctx);
    void Cancel(HLERequestContext& ctx);
    void UpdateFriendInfo(HLERequestContext& ctx);
    void GetFriendProfileImage(HLERequestContext& ctx);
    void GetFriendProfileImageWithImageSize(HLERequestContext& ctx);
    void EnsureFriendListAvailable(HLERequestContext& ctx);
    void SendFriendRequestForApplication(HLERequestContext& ctx);
    void AddFacedFriendRequestForApplication(HLERequestContext& ctx);
    void EnsureBlockedUserListAvailable(HLERequestContext& ctx);
    void GetProfileList(HLERequestContext& ctx);
    void GetProfileListV2(HLERequestContext& ctx);
    void DeclareOpenOnlinePlaySession(HLERequestContext& ctx);
    void GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId(HLERequestContext& ctx);
    void AddPlayHistory(HLERequestContext& ctx);
    void GetProfileImageUrl(HLERequestContext& ctx);
    void GetProfileImageUrlV2(HLERequestContext& ctx);
    void GetFriendDetailedInfo(HLERequestContext& ctx);
    void SyncFriendList(HLERequestContext& ctx);
    void RequestSyncFriendList(HLERequestContext& ctx);
    void GetFriendListForViewer(HLERequestContext& ctx);
    void UpdateFriendInfoForViewer(HLERequestContext& ctx);
    void GetFriendDetailedInfoV2(HLERequestContext& ctx);
    void LoadFriendSetting(HLERequestContext& ctx);
    void GetFriendRequestList(HLERequestContext& ctx);
    void GetFriendRequestListV2(HLERequestContext& ctx);
    void GetFriendCandidateList(HLERequestContext& ctx);
    void GetNintendoNetworkIdInfo(HLERequestContext& ctx);
    void GetSnsAccountLinkage(HLERequestContext& ctx);
    void GetSnsAccountProfile(HLERequestContext& ctx);
    void GetSnsAccountFriendList(HLERequestContext& ctx);
    void GetBlockedUserList(HLERequestContext& ctx);
    void SyncBlockedUserList(HLERequestContext& ctx);
    void GetBlockedUserListV2(HLERequestContext& ctx);
    void GetProfileExtraList(HLERequestContext& ctx);
    void GetRelationship(HLERequestContext& ctx);
    void GetProfileExtraListV2(HLERequestContext& ctx);
    void GetUserPresenceView(HLERequestContext& ctx);
    void GetUserPresenceViewV2(HLERequestContext& ctx);
    void GetPlayHistoryList(HLERequestContext& ctx);
    void GetPlayHistoryListV2(HLERequestContext& ctx);
    void LoadUserSetting(HLERequestContext& ctx);
    void SyncUserSetting(HLERequestContext& ctx);
    void LoadUserSettingV2(HLERequestContext& ctx);
    void RequestListSummaryOverlayNotification(HLERequestContext& ctx);
    void GetExternalApplicationCatalog(HLERequestContext& ctx);
    void GetReceivedFriendInvitationList(HLERequestContext& ctx);
    void GetReceivedFriendInvitationDetailedInfo(HLERequestContext& ctx);
    void GetReceivedFriendInvitationListV2(HLERequestContext& ctx);
    void GetReceivedFriendInvitationDetailedInfoV2(HLERequestContext& ctx);
    void DropFriendNewlyFlags(HLERequestContext& ctx);
    void DeleteFriend(HLERequestContext& ctx);
    void DropFriendNewlyFlag(HLERequestContext& ctx);
    void ChangeFriendFavoriteFlag(HLERequestContext& ctx);
    void ChangeFriendOnlineNotificationFlag(HLERequestContext& ctx);
    void SendFriendRequest(HLERequestContext& ctx);
    void SendFriendRequestWithApplicationInfo(HLERequestContext& ctx);
    void CancelFriendRequest(HLERequestContext& ctx);
    void AcceptFriendRequest(HLERequestContext& ctx);
    void RejectFriendRequest(HLERequestContext& ctx);
    void ReadFriendRequest(HLERequestContext& ctx);
    void GetFacedFriendRequestRegistrationKey(HLERequestContext& ctx);
    void AddFacedFriendRequest(HLERequestContext& ctx);
    void CancelFacedFriendRequest(HLERequestContext& ctx);
    void GetFacedFriendRequestProfileImage(HLERequestContext& ctx);
    void GetFacedFriendRequestProfileImageFromPath(HLERequestContext& ctx);
    void SendFriendRequestWithExternalApplicationCatalogId(HLERequestContext& ctx);
    void ResendFacedFriendRequest(HLERequestContext& ctx);
    void SendFriendRequestWithNintendoNetworkIdInfo(HLERequestContext& ctx);
    void SendFriendRequestWithApplicationInfoV2(HLERequestContext& ctx);
    void GetSnsAccountLinkPageUrl(HLERequestContext& ctx);
    void UnlinkSnsAccount(HLERequestContext& ctx);
    void BlockUser(HLERequestContext& ctx);
    void BlockUserWithApplicationInfo(HLERequestContext& ctx);
    void UnblockUser(HLERequestContext& ctx);
    void BlockUserWithApplicationInfoV2(HLERequestContext& ctx);
    void GetProfileExtraFromFriendCode(HLERequestContext& ctx);
    void GetProfileExtraFromFriendCodeV2(HLERequestContext& ctx);
    void DeletePlayHistory(HLERequestContext& ctx);
    void AddPlayHistoryWithApplication(HLERequestContext& ctx);
    void ChangePresencePermission(HLERequestContext& ctx);
    void ChangeFriendRequestReception(HLERequestContext& ctx);
    void ChangePlayLogPermission(HLERequestContext& ctx);
    void IssueFriendCode(HLERequestContext& ctx);
    void ClearPlayLog(HLERequestContext& ctx);
    void SendFriendInvitation(HLERequestContext& ctx);
    void SendFriendInvitationV2(HLERequestContext& ctx);
    void ReadFriendInvitation(HLERequestContext& ctx);
    void ReadAllFriendInvitations(HLERequestContext& ctx);
    void OpenUser(HLERequestContext& ctx);
    void DeleteFriendListCache(HLERequestContext& ctx);
    void DeleteBlockedUserListCache(HLERequestContext& ctx);
    void DeleteNetworkServiceAccountCache(HLERequestContext& ctx);

    enum class PresenceFilter : u32 {
        None = 0,
        Online = 1,
        OnlinePlay = 2,
        OnlineOrOnlinePlay = 3,
    };

    struct SizedFriendFilter {
        PresenceFilter presence;
        u8 is_favorite;
        u8 same_app;
        u8 same_app_played;
        u8 arbitrary_app_played;
        u64 group_id;
    };
    static_assert(sizeof(SizedFriendFilter) == 0x10, "SizedFriendFilter is an invalid size");

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completion_event;
};

// [Nextendo] Registry of every live INotificationService so NotifyFriendsListUpdated() (called
// from the frontend's background friend-poll thread, not an HLE dispatch thread) can reach them.
// Guarded separately from each instance's own lock so registration/teardown never has to wait on
// whatever that instance's HLE thread happens to be doing.
static std::mutex g_notification_registry_lock;
static std::vector<class INotificationService*> g_notification_services;

class INotificationService final : public ServiceFramework<INotificationService> {
public:
    explicit INotificationService(Core::System& system_, Common::UUID uuid_)
        : ServiceFramework{system_, "INotificationService"}, uuid{uuid_},
          service_context{system_, "INotificationService"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INotificationService::GetEvent, "GetEvent"},
            {1, &INotificationService::Clear, "Clear"},
            {2, &INotificationService::Pop, "Pop"}
        };
        // clang-format on

        RegisterHandlers(functions);

        notification_event = service_context.CreateEvent("INotificationService:NotifyEvent");

        notifications.push(SizedNotificationInfo{NotificationTypes::HasUpdatedFriendsList, {}, 0});
        states.has_updated_friends = true;
        notification_event->Signal();

        std::scoped_lock lk{g_notification_registry_lock};
        g_notification_services.push_back(this);
    }

    ~INotificationService() override {
        {
            std::scoped_lock lk{g_notification_registry_lock};
            std::erase(g_notification_services, this);
        }
        service_context.CloseEvent(notification_event);
    }

    // [Nextendo] Called from NotifyFriendsListUpdated(), potentially from a different thread than
    // this instance's own HLE dispatch -- guarded by instance_lock, unlike the ctor/dtor-only
    // registry lock above.
    void PushFriendsListUpdated() {
        std::scoped_lock lk{instance_lock};
        notifications.push(SizedNotificationInfo{NotificationTypes::HasUpdatedFriendsList, {}, 0});
        states.has_updated_friends = true;
        notification_event->Signal();
    }

private:
    void GetEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");

        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(notification_event->GetReadableEvent());
    }

    void Clear(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");
        std::scoped_lock lk{instance_lock};
        while (!notifications.empty()) {
            notifications.pop();
        }
        std::memset(&states, 0, sizeof(States));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Pop(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");
        std::scoped_lock lk{instance_lock};

        if (notifications.empty()) {
            LOG_ERROR(Service_Friend, "No notifications in queue!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultNoNotifications);
            return;
        }

        const auto notification = notifications.front();
        notifications.pop();

        switch (notification.notification_type) {
        case NotificationTypes::HasUpdatedFriendsList:
            states.has_updated_friends = false;
            break;
        case NotificationTypes::HasReceivedFriendRequest:
            states.has_received_friend_request = false;
            break;
        default:
            // HOS seems not have an error case for an unknown notification
            LOG_WARNING(Service_Friend, "Unknown notification {:08X}",
                        notification.notification_type);
            break;
        }

        IPC::ResponseBuilder rb{ctx, 6};
        rb.Push(ResultSuccess);
        rb.PushRaw<SizedNotificationInfo>(notification);
    }

    enum class NotificationTypes : u32 {
        HasUpdatedFriendsList = 0x65,
        HasReceivedFriendRequest = 0x1
    };

    struct SizedNotificationInfo {
        NotificationTypes notification_type;
        INSERT_PADDING_WORDS(
            1); // TODO (ogniK): This doesn't seem to be used within any IPC returns as of now
        u64_le account_id;
    };
    static_assert(sizeof(SizedNotificationInfo) == 0x10,
                  "SizedNotificationInfo is an incorrect size");

    struct States {
        bool has_updated_friends;
        bool has_received_friend_request;
    };

    Common::UUID uuid;
    KernelHelpers::ServiceContext service_context;

    std::mutex instance_lock;
    Kernel::KEvent* notification_event;
    std::queue<SizedNotificationInfo> notifications;
    States states{};
};

void NotifyFriendsListUpdated() {
    std::scoped_lock lk{g_notification_registry_lock};
    for (auto* service : g_notification_services) {
        service->PushFriendsListUpdated();
    }
}

class IDaemonSuspendSessionService final : public ServiceFramework<IDaemonSuspendSessionService> {
public:
    explicit IDaemonSuspendSessionService(Core::System& system_)
        : ServiceFramework{system_, "IDaemonSuspendSessionService"} {
        // [Zephyron]: No commands for this service, so no handlers to register.
    }

    ~IDaemonSuspendSessionService() override = default;
};

void Module::Interface::CreateFriendService(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IFriendService>(system);
    LOG_DEBUG(Service_Friend, "called");
}

void Module::Interface::CreateNotificationService(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    auto uuid = rp.PopRaw<Common::UUID>();

    LOG_DEBUG(Service_Friend, "called, uuid=0x{}", uuid.RawString());

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INotificationService>(system, uuid);
}

void Module::Interface::CreateDaemonSuspendSessionService(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Friend, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IDaemonSuspendSessionService>(system);
}

Module::Interface::Interface(std::shared_ptr<Module> module_, Core::System& system_,
                             const char* name)
    : ServiceFramework{system_, name}, module{std::move(module_)} {}

Module::Interface::~Interface() = default;

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    auto module = std::make_shared<Module>();

    server_manager->RegisterNamedService("friend:a",
                                         std::make_shared<Friend>(module, system, "friend:a"));
    server_manager->RegisterNamedService("friend:m",
                                         std::make_shared<Friend>(module, system, "friend:m"));
    server_manager->RegisterNamedService("friend:s",
                                         std::make_shared<Friend>(module, system, "friend:s"));
    server_manager->RegisterNamedService("friend:u",
                                         std::make_shared<Friend>(module, system, "friend:u"));
    server_manager->RegisterNamedService("friend:v",
                                         std::make_shared<Friend>(module, system, "friend:v"));

    ServerManager::RunServer(std::move(server_manager));
}

void IFriendService::GetFriendList(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto friend_offset = rp.Pop<u32>();
    const auto uuid = rp.PopRaw<Common::UUID>();
    [[maybe_unused]] const auto filter = rp.PopRaw<IFriendService::SizedFriendFilter>();
    const auto pid = rp.Pop<u64>();
    u32 count = 0;
    if (friend_offset == 0 && ctx.CanWriteBuffer()) {
        const auto entries = Common::NextendoFriends::Get();
        const auto capacity = ctx.GetWriteBufferNumElements<FriendImpl>();
        std::vector<FriendImpl> list;
        for (const auto& entry : entries) {
            if (list.size() >= capacity) {
                break;
            }
            list.push_back(MakeFriend(entry));
        }
        if (!list.empty()) {
            ctx.WriteBuffer(list);
        }
        count = static_cast<u32>(list.size());
    }

    LOG_INFO(Service_Friend, "[Nextendo] GetFriendList offset={} uuid=0x{} pid={} -> {}",
             friend_offset, uuid.RawString(), pid, count);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(count);
}

void IFriendService::CheckFriendListAvailability(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid{rp.PopRaw<Common::UUID>()};
    LOG_WARNING(Service_Friend, "(STUBBED) CheckFriendListAvailability called, uuid=0x{}", uuid.RawString());
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(true);
}

void IFriendService::GetBlockedUserListIds(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetBlockedUserListIds called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Indicates there are no blocked users
}

void IFriendService::CheckBlockedUserListAvailability(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid{rp.PopRaw<Common::UUID>()};
    LOG_WARNING(Service_Friend, "(STUBBED) CheckBlockedUserListAvailability called, uuid=0x{}", uuid.RawString());
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(true);
}

void IFriendService::DeclareCloseOnlinePlaySession(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) DeclareCloseOnlinePlaySession called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::UpdateUserPresence(HLERequestContext& ctx) {
    if (ctx.CanReadBuffer() && ctx.GetReadBufferSize() >= sizeof(UserPresenceImpl)) {
        UserPresenceImpl presence{};
        std::memcpy(&presence, ctx.ReadBuffer().data(), sizeof(presence));
        // A title can publish Offline while it is still running -- MK8D does, on leaving the
        // online menu. Relaying that verbatim reports us offline mid-session, so floor it at
        // Online: this handler only runs while a game is up. The app_field is taken as-is,
        // since that is the title's own joinable-session data.
        auto status = std::max<s32>(static_cast<s32>(presence.status),
                                    Common::NextendoFriends::PresenceOnline);
        auto app_field =
            std::string{reinterpret_cast<const char*>(presence.app_key_value.data()),
                        presence.app_key_value.size()};
        if (FixupInGameFlag(app_field)) {
            status = std::max<s32>(status, Common::NextendoFriends::PresenceOnlinePlay);
            LOG_INFO(Service_Friend,
                     "[Nextendo] Corrected InGame=0->1 in presence blob for a resolved private "
                     "battle host (natf/natm confirmed via NAT-check)");
        } else if (IsArmsSessionActive(app_field)) {
            status = std::max<s32>(status, Common::NextendoFriends::PresenceOnlinePlay);
            LOG_INFO(Service_Friend,
                     "[Nextendo] ARMS JoinMode indicates an active session; bumping status to "
                     "OnlinePlay (raw status floors at Online otherwise)");
        }
        Common::NextendoFriends::SetLocalPresence(status, app_field);
        LOG_INFO(Service_Friend, "[Nextendo] UpdateUserPresence status={} -> {} app_field={}",
                 presence.status, status,
                 Common::HexToString(
                     std::span{reinterpret_cast<const u8*>(app_field.data()), app_field.size()},
                     false));
    } else {
        LOG_WARNING(Service_Friend, "UpdateUserPresence called with no presence buffer");
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetPlayHistoryRegistrationKey(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto local_play = rp.Pop<bool>();
    const auto uuid = rp.PopRaw<Common::UUID>();
    LOG_WARNING(Service_Friend, "(STUBBED) GetPlayHistoryRegistrationKey called, local_play={}, uuid=0x{}", local_play,
                uuid.RawString());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetFriendCount(HLERequestContext& ctx) {
    const auto count = static_cast<u32>(Common::NextendoFriends::Get().size());
    LOG_INFO(Service_Friend, "[Nextendo] GetFriendCount -> {}", count);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(count);
}

void IFriendService::GetNewlyFriendCount(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Friend, "(STUBBED) GetNewlyFriendCount called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(0);
}

void IFriendService::GetReceivedFriendRequestCount(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Friend, "(STUBBED) GetReceivedFriendRequestCount called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(0);
}

void IFriendService::GetPlayHistoryStatistics(HLERequestContext& ctx) {
    LOG_ERROR(Service_Friend, "(STUBBED) GetPlayHistoryStatistics called, check in out");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::Cancel(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) Cancel called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::UpdateFriendInfo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();

    // [Nextendo] This used to return Success while writing NOTHING into its out buffer. Any game
    // that resolves friend details this way -- taking the ids GetFriendListIds already handed it
    // and asking here for the actual name/presence/app-field data -- got the ids back with zero
    // real data, so the friends screen stayed empty even though the id list itself was correct.
    // Matches a real, confirmed Ryujinx-Nextendo bug for this exact method (Splatoon 3's "Amis"
    // screen staying empty despite GetFriendListIds already succeeding). The requested ids are
    // the INPUT here and the resolved details the OUTPUT, one entry per requested id, in the SAME
    // order -- an id we can't resolve gets a zeroed (is_valid=0) entry rather than being skipped,
    // otherwise the caller mis-pairs the remaining ids with the wrong friends.
    // [Nextendo] Defensive: only trust an input buffer that's actually present, and hard-cap the
    // element count regardless of what the size math says -- a previous version of this fix
    // trusted ctx.ReadBuffer(0)'s size unconditionally and hung the emulator (unbounded
    // std::vector<u64> allocation) the first time this call's real buffer layout didn't match
    // what was assumed. A real friend list can never exceed a few hundred entries.
    constexpr std::size_t MaxRequestedIds = 300;
    std::vector<u64> requested_ids;
    if (ctx.CanReadBuffer(0)) {
        const auto requested_ids_raw = ctx.ReadBuffer(0);
        const auto id_count = std::min(requested_ids_raw.size() / sizeof(u64), MaxRequestedIds);
        requested_ids.resize(id_count);
        if (id_count > 0) {
            std::memcpy(requested_ids.data(), requested_ids_raw.data(), id_count * sizeof(u64));
        }
    }

    const auto entries = Common::NextendoFriends::Get();
    const auto capacity = std::min(ctx.GetWriteBufferNumElements<FriendImpl>(), MaxRequestedIds);
    const auto out_count = std::min(requested_ids.size(), capacity);

    std::vector<FriendImpl> info(out_count);
    std::size_t resolved_count = 0;
    for (std::size_t i = 0; i < out_count; ++i) {
        const auto wanted = requested_ids[i];
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [wanted](const auto& e) { return FriendMatchesId(e, wanted); });
        info[i] = it != entries.end() ? MakeFriend(*it, wanted, &uuid) : FriendImpl{};
        if (it != entries.end()) {
            ++resolved_count;
        } else {
            LOG_WARNING(Service_Friend, "UpdateFriendInfo unknown account id={:016X}", wanted);
        }
    }

    if (!info.empty()) {
        ctx.WriteBuffer(info);
    }

    LOG_INFO(Service_Friend, "[Nextendo] UpdateFriendInfo uuid=0x{} requested={} -> {} resolved",
             uuid.RawString(), requested_ids.size(), resolved_count);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

namespace {

// [Nextendo] The caller identifies the target friend by the same synthetic Uid FriendImpl handed
// it (see UidForPid above) -- reverse it back to the raw pid so we can look the entry up in the
// cache. Exact inverse of UidForPid: the low 8 bytes are the pid, verbatim.
u64 PidForUid(const Common::UUID& uid) {
    u64 pid{};
    std::memcpy(&pid, uid.uuid.data(), sizeof(pid));
    return pid;
}

} // namespace

// Plain 256x256 grey icon, the size hardware friend icons are served at.
constexpr std::array<u8, 667> DefaultFriendIcon{
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x05, 0x03, 0x04, 0x04, 0x04, 0x03, 0x05,
    0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x06, 0x07, 0x0C, 0x08, 0x07, 0x07, 0x07, 0x07, 0x0F, 0x0B,
    0x0B, 0x09, 0x0C, 0x11, 0x0F, 0x12, 0x12, 0x11, 0x0F, 0x11, 0x11, 0x13, 0x16, 0x1C, 0x17, 0x13,
    0x14, 0x1A, 0x15, 0x11, 0x11, 0x18, 0x21, 0x18, 0x1A, 0x1D, 0x1D, 0x1F, 0x1F, 0x1F, 0x13, 0x17,
    0x22, 0x24, 0x22, 0x1E, 0x24, 0x1C, 0x1E, 0x1F, 0x1E, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x05, 0x05,
    0x05, 0x07, 0x06, 0x07, 0x0E, 0x08, 0x08, 0x0E, 0x1E, 0x14, 0x11, 0x14, 0x1E, 0x1E, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0xFF, 0xC0,
    0x00, 0x11, 0x08, 0x01, 0x00, 0x01, 0x00, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
    0x01, 0xFF, 0xC4, 0x00, 0x15, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xC4,
    0x00, 0x14, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01,
    0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFF, 0xD9};

void IFriendService::GetFriendProfileImage(HLERequestContext& ctx) {
    // [Nextendo] Was a hardcoded zero-size stub, so every friend tile fell back to the "?"
    // placeholder icon regardless of whether the account server actually had a picture for them.
    // NextendoFriends::Entry now carries the already-decoded JPEG bytes (see PollFriends), so
    // this just has to find the right entry and hand them back.
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();
    // nnSdk sends the local owner's Uid, then the friend's account id.
    const u64 account_id = rp.Pop<u64>();
    const auto pid = account_id != 0 ? account_id : PidForUid(uuid);

    const auto entries = Common::NextendoFriends::Get();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [pid](const auto& e) { return FriendMatchesId(e, pid); });

    // Hardware always answers with an icon (the default one if none is set); Classics aborts on 0.
    u32 size = 0;
    if (it != entries.end() && !it->image.empty()) {
        ctx.WriteBuffer(it->image);
        size = static_cast<u32>(it->image.size());
    } else {
        ctx.WriteBuffer(DefaultFriendIcon);
        size = static_cast<u32>(DefaultFriendIcon.size());
    }

    LOG_INFO(Service_Friend, "[Nextendo] GetFriendProfileImage uuid=0x{} -> {} bytes",
             uuid.RawString(), size);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(size);
}

void IFriendService::GetFriendProfileImageWithImageSize(HLERequestContext& ctx) {
    // [Nextendo] Same lookup as GetFriendProfileImage; the requested/max size argument is
    // advisory on real hardware (a resize hint), and the account server only ever gives us one
    // resolution anyway, so it's not worth threading through a second image variant for.
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();
    // nnSdk sends the local owner's Uid, then the friend's account id.
    const u64 account_id = rp.Pop<u64>();
    const auto pid = account_id != 0 ? account_id : PidForUid(uuid);

    const auto entries = Common::NextendoFriends::Get();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [pid](const auto& e) { return FriendMatchesId(e, pid); });

    // Hardware always answers with an icon (the default one if none is set); Classics aborts on 0.
    u32 size = 0;
    if (it != entries.end() && !it->image.empty()) {
        ctx.WriteBuffer(it->image);
        size = static_cast<u32>(it->image.size());
    } else {
        ctx.WriteBuffer(DefaultFriendIcon);
        size = static_cast<u32>(DefaultFriendIcon.size());
    }

    LOG_INFO(Service_Friend, "[Nextendo] GetFriendProfileImageWithImageSize uuid=0x{} -> {} bytes",
             uuid.RawString(), size);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(size);
}

void IFriendService::EnsureFriendListAvailable(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) EnsureFriendListAvailable called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::SendFriendRequestForApplication(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SendFriendRequestForApplication called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::AddFacedFriendRequestForApplication(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) AddFacedFriendRequestForApplication called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::EnsureBlockedUserListAvailable(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) EnsureBlockedUserListAvailable called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

namespace {
// [Nextendo] Same ids-in (buffer 0, u64 pids)/structs-out contract as GetFriendDetailedInfo --
// only proven-working pattern for this class of call in this codebase, so mirrored here rather
// than guessing a UUID-keyed format with nothing to test it against.
void RespondProfileList(HLERequestContext& ctx, const char* name) {
    constexpr std::size_t MaxRequestedIds = 300;
    std::vector<u64> requested_ids;
    if (ctx.CanReadBuffer(0)) {
        const auto raw = ctx.ReadBuffer(0);
        const auto id_count = std::min(raw.size() / sizeof(u64), MaxRequestedIds);
        requested_ids.resize(id_count);
        if (id_count > 0) {
            std::memcpy(requested_ids.data(), raw.data(), id_count * sizeof(u64));
        }
    }

    const auto capacity = std::min(ctx.GetWriteBufferNumElements<ProfileImpl>(), MaxRequestedIds);
    std::vector<ProfileImpl> profiles;
    std::size_t resolved = 0;
    for (const auto pid : requested_ids) {
        if (profiles.size() >= capacity) {
            break;
        }
        if (const auto profile = MakeProfile(pid)) {
            profiles.push_back(*profile);
            ++resolved;
        } else {
            profiles.push_back(ProfileImpl{});
            LOG_WARNING(Service_Friend, "{} unknown account id={:016X}", name, pid);
        }
    }

    if (!profiles.empty()) {
        ctx.WriteBuffer(profiles);
    }

    LOG_INFO(Service_Friend, "[Nextendo] {} requested={} -> {} resolved", name,
             requested_ids.size(), resolved);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(static_cast<u32>(resolved));
}
} // Anonymous namespace

void IFriendService::GetProfileList(HLERequestContext& ctx) {
    RespondProfileList(ctx, "GetProfileList");
}

void IFriendService::GetProfileListV2(HLERequestContext& ctx) {
    RespondProfileList(ctx, "GetProfileListV2");
}

void IFriendService::DeclareOpenOnlinePlaySession(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) DeclareOpenOnlinePlaySession called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
}

void IFriendService::AddPlayHistory(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) AddPlayHistory called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

constexpr size_t MAX_URL_SIZE = 256;

struct UrlPayload { char url[MAX_URL_SIZE]; };
struct FriendSettingPayload { unsigned char data[1]; };
struct NintendoNetworkIdInfoPayload { unsigned char data[8]; };
struct SnsAccountLinkagePayload { unsigned char data[8]; };
struct RelationshipPayload { unsigned char data[4]; };
struct UserSettingPayload { unsigned char data[16]; };
struct FacedFriendRequestRegistrationKeyPayload { unsigned char data[16]; };
struct FriendCodePayload { char code[15]; }; // Size 15 for 14 chars + null terminator

namespace {
// nnSdk sends the base image URL (0xA0 bytes) and an ImageSize; one resolution is served, so echo it.
void RespondProfileImageUrl(HLERequestContext& ctx) {
    using Url = std::array<char, 0xA0>;
    IPC::RequestParser rp{ctx};
    const auto url = rp.PopRaw<Url>();
    IPC::ResponseBuilder rb{ctx, 2 + sizeof(Url) / sizeof(u32)};
    rb.Push(ResultSuccess);
    rb.PushRaw(url);
}
} // Anonymous namespace

void IFriendService::GetProfileImageUrl(HLERequestContext& ctx) {
    RespondProfileImageUrl(ctx);
}

void IFriendService::GetProfileImageUrlV2(HLERequestContext& ctx) {
    RespondProfileImageUrl(ctx);
}

void IFriendService::GetFriendDetailedInfo(HLERequestContext& ctx) {
    // [Nextendo] Same ids-in/details-out contract as GetFriendDetailedInfoV2 below (this is the
    // pre-20.0.0 command id for the same viewer-detail lookup).
    constexpr std::size_t MaxRequestedIds = 300;
    std::vector<u64> requested_ids;
    if (ctx.CanReadBuffer(0)) {
        const auto requested_ids_raw = ctx.ReadBuffer(0);
        const auto id_count = std::min(requested_ids_raw.size() / sizeof(u64), MaxRequestedIds);
        requested_ids.resize(id_count);
        if (id_count > 0) {
            std::memcpy(requested_ids.data(), requested_ids_raw.data(), id_count * sizeof(u64));
        }
    }

    const auto entries = Common::NextendoFriends::Get();
    const auto capacity = std::min(ctx.GetWriteBufferNumElements<FriendImpl>(), MaxRequestedIds);
    const auto out_count = std::min(requested_ids.size(), capacity);

    std::vector<FriendImpl> info(out_count);
    for (std::size_t i = 0; i < out_count; ++i) {
        const auto wanted = requested_ids[i];
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [wanted](const auto& e) { return FriendMatchesId(e, wanted); });
        info[i] = it != entries.end() ? MakeFriend(*it, wanted) : FriendImpl{};
    }

    if (!info.empty()) {
        ctx.WriteBuffer(info);
    }

    LOG_INFO(Service_Friend, "[Nextendo] GetFriendDetailedInfo requested={} -> {} resolved",
             requested_ids.size(), out_count);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::SyncFriendList(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SyncFriendList called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::RequestSyncFriendList(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) RequestSyncFriendList called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetFriendListForViewer(HLERequestContext& ctx) {
    // [Nextendo] The count-only version of this call left MyPage's Friends viewer showing a
    // correct "36 friends" header with a permanently empty list underneath -- the same class of
    // bug UpdateFriendInfo's comment above describes for GetFriendListIds. This is the id-buffer
    // half of that same two-step viewer flow (ids here, details resolved per-id via
    // GetFriendDetailedInfoV2 below), so it follows GetFriendListIds's proven pattern exactly.
    // [Nextendo] TEMP: GetFriendListIds (its game-facing sibling) takes an offset scalar first --
    // probe for the same here, in case this call is meant to paginate and my always-return-
    // everything implementation is confusing whatever pagination state the viewer tracks.
    IPC::RequestParser probe_rp{ctx};
    const auto probe_offset = probe_rp.Pop<s32>();
    LOG_INFO(Service_Friend, "[Nextendo] GetFriendListForViewer probe_offset={}", probe_offset);

    u32 count = 0;
    if (ctx.CanWriteBuffer()) {
        const auto entries = Common::NextendoFriends::Get();
        const auto capacity = ctx.GetWriteBufferNumElements<u64>();
        std::vector<u64> ids;
        for (const auto& entry : entries) {
            if (ids.size() >= capacity) {
                break;
            }
            ids.push_back(FriendAdvertisedId(entry));
        }
        if (!ids.empty()) {
            ctx.WriteBuffer(ids);
        }
        count = static_cast<u32>(ids.size());
    }

    // [Nextendo] TEMP: dump the actual ids handed out here so we can diff them against what
    // UpdateFriendInfoForViewer later receives back -- matched=1/36 there means the guest is
    // requesting a mostly-different id set than this call just gave it.
    std::string ids_dump;
    if (ctx.CanWriteBuffer()) {
        const auto entries = Common::NextendoFriends::Get();
        for (std::size_t i = 0; i < std::min<std::size_t>(entries.size(), 5); ++i) {
            ids_dump += fmt::format("{} ", entries[i].pid);
        }
    }
    LOG_INFO(Service_Friend, "[Nextendo] GetFriendListForViewer -> {} first5=[{}]", count,
             ids_dump);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(count);
}

void IFriendService::UpdateFriendInfoForViewer(HLERequestContext& ctx) {
    // [Nextendo] This, not GetFriendDetailedInfoV2, turns out to be what MyPage's Friends viewer
    // actually polls per-tile (GetFriendDetailedInfoV2 was only ever seen called with an empty id
    // buffer). Same ids-in/details-out contract as UpdateFriendInfo/GetFriendDetailedInfo(V2).
    constexpr std::size_t MaxRequestedIds = 300;
    std::vector<u64> requested_ids;
    if (ctx.CanReadBuffer(0)) {
        const auto requested_ids_raw = ctx.ReadBuffer(0);
        const auto id_count = std::min(requested_ids_raw.size() / sizeof(u64), MaxRequestedIds);
        requested_ids.resize(id_count);
        if (id_count > 0) {
            std::memcpy(requested_ids.data(), requested_ids_raw.data(), id_count * sizeof(u64));
        }
    }

    const auto entries = Common::NextendoFriends::Get();
    // [Nextendo] TEMP: raw byte capacity of the output buffer the guest actually allocated. If
    // this isn't a multiple of sizeof(FriendImpl)==0x200, this call's real output struct isn't
    // FriendImpl at all -- would explain why even a successfully-resolved entry never renders.
    const auto raw_write_size = ctx.GetWriteBufferSize();
    const auto capacity = std::min(ctx.GetWriteBufferNumElements<FriendImpl>(), MaxRequestedIds);
    const auto out_count = std::min(requested_ids.size(), capacity);

    std::vector<FriendImpl> info(out_count);
    std::size_t matched_count = 0;
    for (std::size_t i = 0; i < out_count; ++i) {
        const auto wanted = requested_ids[i];
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [wanted](const auto& e) { return FriendMatchesId(e, wanted); });
        if (it != entries.end()) {
            info[i] = MakeFriend(*it);
            ++matched_count;
        } else {
            info[i] = FriendImpl{};
        }
    }

    if (!info.empty()) {
        ctx.WriteBuffer(info);
    }

    LOG_INFO(Service_Friend,
             "[Nextendo] UpdateFriendInfoForViewer raw_write_size={} (sizeof(FriendImpl)={}, "
             "{:.2f} entries worth)",
             raw_write_size, sizeof(FriendImpl),
             static_cast<double>(raw_write_size) / sizeof(FriendImpl));

    // [Nextendo] TEMP: dump the actual requested ids to diff against GetFriendListForViewer's
    // first5 -- "resolved" alone only ever meant "slots written", not "slots that found a real
    // friend", so it could say 36 even if find_if missed on every single one.
    std::string req_dump;
    for (std::size_t i = 0; i < std::min<std::size_t>(requested_ids.size(), 5); ++i) {
        req_dump += fmt::format("{} ", requested_ids[i]);
    }
    LOG_INFO(Service_Friend,
             "[Nextendo] UpdateFriendInfoForViewer requested={} first5=[{}] matched={}/{} "
             "cache_size={}",
             requested_ids.size(), req_dump, matched_count, out_count, entries.size());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetFriendDetailedInfoV2(HLERequestContext& ctx) {
    // [Nextendo] Same ids-in/details-out contract as UpdateFriendInfo above (one resolved entry
    // per requested id, in the same order, zeroed rather than skipped when an id can't be
    // resolved) -- this is the detail half of the GetFriendListForViewer flow MyPage's Friends
    // viewer actually uses, so without this the viewer had ids but nothing to show for them.
    constexpr std::size_t MaxRequestedIds = 300;
    std::vector<u64> requested_ids;
    if (ctx.CanReadBuffer(0)) {
        const auto requested_ids_raw = ctx.ReadBuffer(0);
        const auto id_count = std::min(requested_ids_raw.size() / sizeof(u64), MaxRequestedIds);
        requested_ids.resize(id_count);
        if (id_count > 0) {
            std::memcpy(requested_ids.data(), requested_ids_raw.data(), id_count * sizeof(u64));
        }
    }

    const auto entries = Common::NextendoFriends::Get();
    const auto capacity = std::min(ctx.GetWriteBufferNumElements<FriendImpl>(), MaxRequestedIds);
    const auto out_count = std::min(requested_ids.size(), capacity);

    std::vector<FriendImpl> info(out_count);
    for (std::size_t i = 0; i < out_count; ++i) {
        const auto wanted = requested_ids[i];
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [wanted](const auto& e) { return FriendMatchesId(e, wanted); });
        info[i] = it != entries.end() ? MakeFriend(*it, wanted) : FriendImpl{};
    }

    if (!info.empty()) {
        ctx.WriteBuffer(info);
    }

    LOG_INFO(Service_Friend, "[Nextendo] GetFriendDetailedInfoV2 requested={} -> {} resolved",
             requested_ids.size(), out_count);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::LoadFriendSetting(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) LoadFriendSetting called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    FriendSettingPayload payload{{0}};
    rb.PushRaw(payload);
}

void IFriendService::GetFriendRequestList(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetFriendRequestList called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Request count
}

void IFriendService::GetFriendRequestListV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetFriendRequestListV2 called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Request count
}

void IFriendService::GetFriendCandidateList(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetFriendCandidateList called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Candidate count
}

void IFriendService::GetNintendoNetworkIdInfo(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetNintendoNetworkIdInfo called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    NintendoNetworkIdInfoPayload payload{{0}};
    rb.PushRaw(payload);
}

void IFriendService::GetSnsAccountLinkage(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetSnsAccountLinkage called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    SnsAccountLinkagePayload payload{{0}};
    rb.PushRaw(payload);
}

void IFriendService::GetSnsAccountProfile(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetSnsAccountProfile called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
    // TODO [Zephyron]: Needs buffer for SnsAccountProfile (X, (pid, size))
}

void IFriendService::GetSnsAccountFriendList(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetSnsAccountFriendList called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Friend count
}

void IFriendService::GetBlockedUserList(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetBlockedUserList called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Blocked user count
}

void IFriendService::SyncBlockedUserList(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SyncBlockedUserList called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetBlockedUserListV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetBlockedUserListV2 called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Blocked user count
}

void IFriendService::GetProfileExtraList(HLERequestContext& ctx) {
    RespondProfileList(ctx, "GetProfileExtraList");
}

void IFriendService::GetRelationship(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetRelationship called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    RelationshipPayload payload{{0}};
    rb.PushRaw(payload);
}

void IFriendService::GetProfileExtraListV2(HLERequestContext& ctx) {
    RespondProfileList(ctx, "GetProfileExtraListV2");
}

void IFriendService::GetUserPresenceView(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetUserPresenceView called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
    // TODO [Zephyron]: Needs buffer for UserPresenceView (X, (pid, size))
}

void IFriendService::GetUserPresenceViewV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetUserPresenceViewV2 called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
    // TODO [Zephyron]: Needs buffer for UserPresenceView (X, (pid, size))
}

void IFriendService::GetPlayHistoryList(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetPlayHistoryList called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Count
}

void IFriendService::GetPlayHistoryListV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetPlayHistoryListV2 called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Count
}

void IFriendService::LoadUserSetting(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) LoadUserSetting called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    UserSettingPayload payload{{0}};
    rb.PushRaw(payload);
}

void IFriendService::SyncUserSetting(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SyncUserSetting called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::LoadUserSettingV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) LoadUserSettingV2 called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    UserSettingPayload payload{{0}};
    rb.PushRaw(payload);
}

void IFriendService::RequestListSummaryOverlayNotification(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) RequestListSummaryOverlayNotification called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetExternalApplicationCatalog(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetExternalApplicationCatalog called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Count
}

void IFriendService::GetReceivedFriendInvitationList(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetReceivedFriendInvitationList called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Count
}

void IFriendService::GetReceivedFriendInvitationDetailedInfo(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetReceivedFriendInvitationDetailedInfo called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetReceivedFriendInvitationListV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetReceivedFriendInvitationListV2 called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Count
}

void IFriendService::GetReceivedFriendInvitationDetailedInfoV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetReceivedFriendInvitationDetailedInfoV2 called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::DropFriendNewlyFlags(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) DropFriendNewlyFlags called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::DeleteFriend(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) DeleteFriend called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::DropFriendNewlyFlag(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) DropFriendNewlyFlag called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::ChangeFriendFavoriteFlag(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ChangeFriendFavoriteFlag called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::ChangeFriendOnlineNotificationFlag(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ChangeFriendOnlineNotificationFlag called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::SendFriendRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SendFriendRequest called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::SendFriendRequestWithApplicationInfo(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SendFriendRequestWithApplicationInfo called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::CancelFriendRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) CancelFriendRequest called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::AcceptFriendRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) AcceptFriendRequest called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::RejectFriendRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) RejectFriendRequest called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::ReadFriendRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ReadFriendRequest called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetFacedFriendRequestRegistrationKey(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetFacedFriendRequestRegistrationKey called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    FacedFriendRequestRegistrationKeyPayload payload{{0}};
    rb.PushRaw(payload);
}

void IFriendService::AddFacedFriendRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) AddFacedFriendRequest called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::CancelFacedFriendRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) CancelFacedFriendRequest called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetFacedFriendRequestProfileImage(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetFacedFriendRequestProfileImage called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Image size
}

void IFriendService::GetFacedFriendRequestProfileImageFromPath(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetFacedFriendRequestProfileImageFromPath called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Image size
}

void IFriendService::SendFriendRequestWithExternalApplicationCatalogId(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SendFriendRequestWithExternalApplicationCatalogId called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::ResendFacedFriendRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ResendFacedFriendRequest called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::SendFriendRequestWithNintendoNetworkIdInfo(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SendFriendRequestWithNintendoNetworkIdInfo called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::SendFriendRequestWithApplicationInfoV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SendFriendRequestWithApplicationInfoV2 called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetSnsAccountLinkPageUrl(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetSnsAccountLinkPageUrl called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    UrlPayload dummy_url_payload{{0}};
    rb.PushRaw(dummy_url_payload);
}

void IFriendService::UnlinkSnsAccount(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) UnlinkSnsAccount called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::BlockUser(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) BlockUser called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::BlockUserWithApplicationInfo(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) BlockUserWithApplicationInfo called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::UnblockUser(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) UnblockUser called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::BlockUserWithApplicationInfoV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) BlockUserWithApplicationInfoV2 called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetProfileExtraFromFriendCode(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetProfileExtraFromFriendCode called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::GetProfileExtraFromFriendCodeV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) GetProfileExtraFromFriendCodeV2 called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::DeletePlayHistory(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) DeletePlayHistory called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::AddPlayHistoryWithApplication(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) AddPlayHistoryWithApplication called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::ChangePresencePermission(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ChangePresencePermission called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::ChangeFriendRequestReception(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ChangeFriendRequestReception called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::ChangePlayLogPermission(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ChangePlayLogPermission called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::IssueFriendCode(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) IssueFriendCode called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    FriendCodePayload payload{}; // Default initialize
    strncpy(payload.code, "0000-0000-0000", sizeof(payload.code) -1 );
    payload.code[sizeof(payload.code) - 1] = '\0'; // Ensure null termination for safety
    rb.PushRaw(payload);
}

void IFriendService::ClearPlayLog(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ClearPlayLog called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::SendFriendInvitation(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SendFriendInvitation called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::SendFriendInvitationV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) SendFriendInvitationV2 called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::ReadFriendInvitation(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ReadFriendInvitation called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::ReadAllFriendInvitations(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) ReadAllFriendInvitations called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::OpenUser(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) OpenUser called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::DeleteFriendListCache(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) DeleteFriendListCache called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::DeleteBlockedUserListCache(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) DeleteBlockedUserListCache called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IFriendService::DeleteNetworkServiceAccountCache(HLERequestContext& ctx) {
    LOG_WARNING(Service_Friend, "(STUBBED) DeleteNetworkServiceAccountCache called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

} // namespace Service::Friend
