// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "common/cityhash.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nifm/nifm.h"
#include "core/hle/service/server_manager.h"
#include "network/network.h"

namespace {

// Avoids name conflict with Windows' CreateEvent macro.
[[nodiscard]] Kernel::KEvent* CreateKEvent(Service::KernelHelpers::ServiceContext& service_context,
                                           std::string&& name) {
    return service_context.CreateEvent(std::move(name));
}

} // Anonymous namespace

#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"

// [UNITY-FIX] undef Win32 macros shadowing ServiceContext methods.
#undef CreateEvent
#undef CreateMutex
#undef CreateSemaphore

namespace Service::NIFM {

// This is nn::nifm::RequestState
// Reference: https://switchbrew.org/wiki/Network_Interface_services#RequestState
enum class RequestState : u32 {
    Invalid = 0,
    Free = 1, ///< NotSubmitted/Free state
    OnHold = 2,
    Accepted = 3,
    Blocking = 4,
};

// This is nn::nifm::NetworkInterfaceType
enum class NetworkInterfaceType : u32 {
    Invalid = 0,
    WiFi_Ieee80211 = 1,
    Ethernet = 2,
};

enum class InternetConnectionStatus : u8 {
    ConnectingUnknown1,
    ConnectingUnknown2,
    ConnectingUnknown3,
    ConnectingUnknown4,
    Connected,
};

// This is nn::nifm::NetworkProfileType
enum class NetworkProfileType : u32 {
    User = 1,
    SsidList = 2,
    Temporary = 4,
};

// This is nn::nifm::ConnectionConfirmationOption
// Reference: https://switchbrew.org/wiki/Network_Interface_services#ConnectionConfirmationOption
enum class ConnectionConfirmationOption : u32 {
    Invalid = 0,
    Prohibited = 1,
    NotRequired = 2,
    Preferred = 3,
    Required = 4,
    Forced = 5,
};

// This is nn::nifm::IpAddressSetting
struct IpAddressSetting {
    bool is_automatic{};
    Network::IPv4Address ip_address{};
    Network::IPv4Address subnet_mask{};
    Network::IPv4Address default_gateway{};
};
static_assert(sizeof(IpAddressSetting) == 0xD, "IpAddressSetting has incorrect size.");

// This is nn::nifm::DnsSetting
struct DnsSetting {
    bool is_automatic{};
    Network::IPv4Address primary_dns{};
    Network::IPv4Address secondary_dns{};
};
static_assert(sizeof(DnsSetting) == 0x9, "DnsSetting has incorrect size.");

// This is nn::nifm::AuthenticationSetting
struct AuthenticationSetting {
    bool is_enabled{};
    std::array<char, 0x20> user{};
    std::array<char, 0x20> password{};
};
static_assert(sizeof(AuthenticationSetting) == 0x41, "AuthenticationSetting has incorrect size.");

// This is nn::nifm::ProxySetting
struct ProxySetting {
    bool is_enabled{};
    INSERT_PADDING_BYTES(1);
    u16 port{};
    std::array<char, 0x64> proxy_server{};
    AuthenticationSetting authentication{};
    INSERT_PADDING_BYTES(1);
};
static_assert(sizeof(ProxySetting) == 0xAA, "ProxySetting has incorrect size.");

// This is nn::nifm::IpSettingData
struct IpSettingData {
    IpAddressSetting ip_address_setting{};
    DnsSetting dns_setting{};
    ProxySetting proxy_setting{};
    u16 mtu{};
};
static_assert(sizeof(IpSettingData) == 0xC2, "IpSettingData has incorrect size.");

struct SfWirelessSettingData {
    u8 ssid_length{};
    std::array<char, 0x20> ssid{};
    u8 unknown_1{};
    u8 unknown_2{};
    u8 unknown_3{};
    std::array<char, 0x41> passphrase{};
};
static_assert(sizeof(SfWirelessSettingData) == 0x65, "SfWirelessSettingData has incorrect size.");

// Verified against the local HOME SDK: command 36 writes 0x34 bytes;
// 0x2c is converted to the signal enum and 0x2e to a signed channel.
struct SfAccessPointData {
    u8 ssid_length{};
    std::array<char, 32> ssid{};
    std::array<u8, 6> bssid{};
    u8 padding{};
    s32 rssi{};
    u8 signal_level{};
    u8 padding_2{};
    u16 channel{};
    std::array<u8, 4> reserved{};
};
static_assert(sizeof(SfAccessPointData) == 0x34);
static_assert(offsetof(SfAccessPointData, signal_level) == 0x2c);
static_assert(offsetof(SfAccessPointData, channel) == 0x2e);

// Windows DOT11 enum -> NIFM values observed in HOME's security selector.
// Unsupported enterprise/WPA3 modes stay unknown rather than claiming WPA2.
static void SetHostWirelessSecurity(SfWirelessSettingData& data,
                                    const Network::NetworkInterface& iface) {
    if (!iface.wireless_connection_available) {
        return;
    }
    switch (iface.wireless_authentication) {
    case 1: data.unknown_2 = 1; break; // Open
    case 2: data.unknown_2 = 2; break; // Shared-key
    case 4: data.unknown_2 = 4; break; // WPA-PSK
    case 7: data.unknown_2 = 6; break; // RSNA-PSK (WPA2)
    default: break;
    }
    switch (iface.wireless_cipher) {
    case 0: data.unknown_3 = 1; break; // None
    case 1:
    case 5:
    case 0x101: data.unknown_3 = 2; break; // WEP
    case 4: data.unknown_3 = 4; break; // CCMP/AES
    default: break;
    }
}

struct NifmWirelessSettingData {
    u8 ssid_length{};
    std::array<char, 0x21> ssid{};
    u8 unknown_1{};
    INSERT_PADDING_BYTES(1);
    u32 unknown_2{};
    u32 unknown_3{};
    std::array<char, 0x41> passphrase{};
    INSERT_PADDING_BYTES(3);
};
static_assert(sizeof(NifmWirelessSettingData) == 0x70,
              "NifmWirelessSettingData has incorrect size.");

#pragma pack(push, 1)
// This is nn::nifm::detail::sf::NetworkProfileData
struct SfNetworkProfileData {
    IpSettingData ip_setting_data{};
    u128 uuid{};
    std::array<char, 0x40> network_name{};
    u8 unknown_1{};
    u8 unknown_2{};
    u8 unknown_3{};
    u8 unknown_4{};
    SfWirelessSettingData wireless_setting_data{};
    INSERT_PADDING_BYTES(1);
};
static_assert(sizeof(SfNetworkProfileData) == 0x17C, "SfNetworkProfileData has incorrect size.");

// This is nn::nifm::NetworkProfileData
struct NifmNetworkProfileData {
    u128 uuid{};
    std::array<char, 0x40> network_name{};
    NetworkProfileType network_profile_type{};
    NetworkInterfaceType network_interface_type{};
    bool is_auto_connect{};
    bool is_large_capacity{};
    INSERT_PADDING_BYTES(2);
    NifmWirelessSettingData wireless_setting_data{};
    IpSettingData ip_setting_data{};
};
static_assert(sizeof(NifmNetworkProfileData) == 0x18E,
              "NifmNetworkProfileData has incorrect size.");
#pragma pack(pop)

[[maybe_unused]] constexpr Result ResultPendingConnection{ErrorModule::NIFM, 111};
constexpr Result ResultNetworkCommunicationDisabled{ErrorModule::NIFM, 1111};

class IScanRequest final : public ServiceFramework<IScanRequest> {
public:
    explicit IScanRequest(Core::System& system_)
        : ServiceFramework{system_, "IScanRequest"}, service_context{system_, "IScanRequest"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IScanRequest::Submit, "Submit"},
            {1, &IScanRequest::IsProcessing, "IsProcessing"},
            {2, &IScanRequest::GetResult, "GetResult"},
            {3, &IScanRequest::GetSystemEventReadableHandle, "GetSystemEventReadableHandle"},
            {4, &IScanRequest::SetChannels, "SetChannels"},
        };
        // clang-format on

        RegisterHandlers(functions);

        event = CreateKEvent(service_context, "IScanRequest:Event");
    }

    ~IScanRequest() override {
        service_context.CloseEvent(event);
    }

private:
    void Submit(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsProcessing(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u8>(0); // Not processing
    }

    void GetResult(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetSystemEventReadableHandle(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(event->GetReadableEvent());
    }

    void SetChannels(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event;
};

class IRequest final : public ServiceFramework<IRequest> {
public:
    explicit IRequest(Core::System& system_)
        : ServiceFramework{system_, "IRequest"}, service_context{system_, "IRequest"} {
        static const FunctionInfo functions[] = {
            {0, &IRequest::GetRequestState, "GetRequestState"},
            {1, &IRequest::GetResult, "GetResult"},
            {2, &IRequest::GetSystemEventReadableHandles, "GetSystemEventReadableHandles"},
            {3, &IRequest::Cancel, "Cancel"},
            {4, &IRequest::Submit, "Submit"},
            {5, &IRequest::SetRequirement, "SetRequirement"},
            {6, &IRequest::SetRequirementPreset, "SetRequirementPreset"},
            {8, &IRequest::SetPriority, "SetPriority"},
            {9, &IRequest::SetNetworkProfileId, "SetNetworkProfileId"},
            {10, &IRequest::SetRejectable, "SetRejectable"},
            {11, &IRequest::SetConnectionConfirmationOption, "SetConnectionConfirmationOption"},
            {12, &IRequest::SetPersistent, "SetPersistent"},
            {13, &IRequest::SetInstant, "SetInstant"},
            {14, &IRequest::SetSustainable, "SetSustainable"},
            {15, &IRequest::SetRawPriority, "SetRawPriority"},
            {16, &IRequest::SetGreedy, "SetGreedy"},
            {17, &IRequest::SetSharable, "SetSharable"},
            {18, &IRequest::SetRequirementByRevision, "SetRequirementByRevision"},
            {19, &IRequest::GetRequirement, "GetRequirement"},
            {20, &IRequest::GetRevision, "GetRevision"},
            {21, &IRequest::GetAppletInfo, "GetAppletInfo"},
            {22, &IRequest::GetAdditionalInfo, "GetAdditionalInfo"},
            {23, &IRequest::SetKeptInSleep, "SetKeptInSleep"},
            {24, &IRequest::RegisterSocketDescriptor, "RegisterSocketDescriptor"},
            {25, &IRequest::UnregisterSocketDescriptor, "UnregisterSocketDescriptor"},
        };
        RegisterHandlers(functions);

        event1 = CreateKEvent(service_context, "IRequest:Event1");
        event2 = CreateKEvent(service_context, "IRequest:Event2");
        state = RequestState::Free;

        // [Nextendo] ResolveState() (and the event1->Signal() it can trigger via UpdateState)
        // previously only ever ran as a side effect of the GUEST calling GetRequestState/
        // GetResult/Submit again. A title that creates this request, grabs event1's handle, and
        // just waits on it -- exactly the point of an event-based readiness API, avoiding polling
        // -- would see event1 signal at most once (whatever ResolveState() happened to observe at
        // the last explicit call) and never again, even though the real network state can still
        // change afterward. Confirmed live: Splatoon 3's own NPLN-readiness worker thread spins a
        // timeout=0 WaitSynchronization probe on this exact event every ~16ms, forever, without
        // ever re-invoking any IRequest method -- so it can only ever be woken by something
        // proactively re-checking network state in the background, which nothing did. Poll every
        // 100ms (plenty responsive for a state that changes on the order of seconds, nowhere near
        // hot-path frequency) for the lifetime of this request and signal whenever it actually
        // changes, matching what a real console's nifm does continuously regardless of whether the
        // application is actively querying it.
        state_poll_event = Core::Timing::CreateEvent(
            "IRequest::StatePoll", [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                std::scoped_lock lk{state_lock};
                ResolveState();
                return std::nullopt;
            });
        system.CoreTiming().ScheduleLoopingEvent(std::chrono::milliseconds(100),
                                                 std::chrono::milliseconds(100), state_poll_event);
    }

    ~IRequest() override {
        system.CoreTiming().UnscheduleEvent(state_poll_event);
        service_context.CloseEvent(event1);
        service_context.CloseEvent(event2);
    }

private:
    // Resolves immediately; parking in OnHold made titles that poll GetRequestState hang.
    // [Nextendo] Callable from either the IPC-dispatch thread (guest calling GetRequestState/
    // GetResult/Submit) or the CoreTiming thread (state_poll_event above) -- state_lock (held by
    // every caller, see LockService()'s own class comment about exactly this CoreTiming-thread
    // access pattern) keeps the read-modify-write of `state` and the event signal consistent
    // between the two.
    RequestState ResolveState() {
        const auto new_state = Network::GetHostIPv4Address().has_value() ? RequestState::Accepted
                                                                        : RequestState::Free;
        if (state != new_state) {
            UpdateState(new_state);
        }
        return state;
    }

    void Submit(HLERequestContext& ctx) {
        std::scoped_lock lk{state_lock};
        LOG_DEBUG(Service_NIFM, "called, state={}", static_cast<u32>(ResolveState()));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetRequestState(HLERequestContext& ctx) {
        std::scoped_lock lk{state_lock};
        const auto current = ResolveState();

        LOG_DEBUG(Service_NIFM, "called, state={}", static_cast<u32>(current));

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(current);
    }

    void SetRequirementPreset(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto param_1 = rp.Pop<u32>();

        LOG_WARNING(Service_NIFM, "(STUBBED) called, param_1={}", param_1);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetResult(HLERequestContext& ctx) {
        std::scoped_lock lk{state_lock};
        const auto current = ResolveState();
        const auto result = current == RequestState::Accepted ? ResultSuccess
                                                             : ResultNetworkCommunicationDisabled;

        LOG_DEBUG(Service_NIFM, "called, state={}", static_cast<u32>(current));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
    }

    void GetSystemEventReadableHandles(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "called");

        IPC::ResponseBuilder rb{ctx, 2, 2};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(event1->GetReadableEvent(), event2->GetReadableEvent());
    }

    void Cancel(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetConnectionConfirmationOption(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto connection_option = rp.PopEnum<ConnectionConfirmationOption>();

        LOG_INFO(Service_NIFM, "called, connection_option={}", static_cast<u32>(connection_option));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetAppletInfo(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        std::vector<u8> out_buffer(ctx.GetWriteBufferSize());

        ctx.WriteBuffer(out_buffer);

        IPC::ResponseBuilder rb{ctx, 5};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0);
        rb.Push<u32>(0);
        rb.Push<u32>(0);
    }

    void UpdateState(RequestState new_state) {
        state = new_state;
        event1->Signal();
        // [Nextendo] event2 (the second handle GetSystemEventReadableHandles hands the guest) was
        // never signaled anywhere in this file -- a title that waits on it instead of/alongside
        // event1 would wait forever, no matter what the real network state is. Confirmed live:
        // Splatoon 3's own NPLN-readiness worker thread polls exactly this shape of never-signaled
        // event (timeout=0 WaitSynchronization probe, every ~16ms, forever) while event1's own
        // state transition (this same call, moments earlier from Submit()/GetRequestState())
        // already correctly resolved to Accepted. Signal both on every state change until the
        // real, distinct semantics for event2 are known -- strictly better than never firing it at
        // all, and safe: a spurious extra wake on an already-satisfied wait is harmless.
        event2->Signal();
    }

    void SetRequirement(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetPriority(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetNetworkProfileId(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetRejectable(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetPersistent(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetInstant(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetSustainable(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetRawPriority(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetGreedy(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetSharable(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetRequirementByRevision(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetRequirement(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetRevision(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(1); // Arbitrary revision number
    }

    void GetAdditionalInfo(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0); // No additional info
    }

    void SetKeptInSleep(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void RegisterSocketDescriptor(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void UnregisterSocketDescriptor(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext service_context;

    RequestState state;
    std::mutex state_lock;
    std::shared_ptr<Core::Timing::EventType> state_poll_event;

    Kernel::KEvent* event1;
    Kernel::KEvent* event2;
};

class INetworkProfile final : public ServiceFramework<INetworkProfile> {
public:
    explicit INetworkProfile(Core::System& system_) : ServiceFramework{system_, "INetworkProfile"} {
        static const FunctionInfo functions[] = {
            {0, nullptr, "Update"},
            {1, nullptr, "PersistOld"},
            {2, nullptr, "Persist"},
        };
        RegisterHandlers(functions);
    }
};

void IGeneralService::GetClientId(HLERequestContext& ctx) {
    static constexpr u32 client_id = 1;
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<u64>(client_id); // Client ID needs to be non zero otherwise it's considered invalid
}

void IGeneralService::CreateScanRequest(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};

    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IScanRequest>(system);
}

void IGeneralService::CreateRequest(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};

    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IRequest>(system);
}

void IGeneralService::GetCurrentNetworkProfile(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    const auto net_iface = Network::GetSelectedNetworkInterface();

    SfNetworkProfileData network_profile_data = [&net_iface] {
        if (!net_iface) {
            return SfNetworkProfileData{};
        }

        const u64 name_hash = Common::CityHash64(net_iface->name.data(), net_iface->name.size());
        const std::string& ssid = net_iface->ssid;

        const auto to_array = [](auto& array, const std::string& value) {
            const size_t count = std::min(value.size(), array.size());
            std::memcpy(array.data(), value.data(), count);
        };

        SfNetworkProfileData data{
            .ip_setting_data{
                .ip_address_setting{
                    .is_automatic{true},
                    .ip_address{Network::TranslateIPv4(net_iface->ip_address)},
                    .subnet_mask{Network::TranslateIPv4(net_iface->subnet_mask)},
                    .default_gateway{Network::TranslateIPv4(net_iface->gateway)},
                },
                .dns_setting{
                    .is_automatic{true},
                    .primary_dns{Network::TranslateIPv4(net_iface->dns_servers[0])},
                    .secondary_dns{Network::TranslateIPv4(net_iface->dns_servers[1])},
                },
                .proxy_setting{
                    .is_enabled{false},
                    .port{},
                    .proxy_server{},
                    .authentication{
                        .is_enabled{},
                        .user{},
                        .password{},
                    },
                },
                .mtu{1500},
            },
            .uuid{name_hash, name_hash},
        };
        to_array(data.network_name, ssid.empty() ? net_iface->name : ssid);
        data.unknown_1 = static_cast<u8>(NetworkProfileType::User);
        data.unknown_2 = static_cast<u8>(net_iface->wireless ? NetworkInterfaceType::WiFi_Ieee80211 : NetworkInterfaceType::Ethernet);
        data.unknown_3 = 1;
        data.wireless_setting_data.ssid_length = static_cast<u8>(std::min(ssid.size(), size_t{32}));
        to_array(data.wireless_setting_data.ssid, ssid);
        SetHostWirelessSecurity(data.wireless_setting_data, *net_iface);
        return data;
    }();

    // When we're connected to a room, spoof the hosts IP address
    if (auto room_member = network.GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            network_profile_data.ip_setting_data.ip_address_setting.ip_address =
                room_member->GetFakeIpAddress();
        }
    }

    ctx.WriteBuffer(network_profile_data);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::EnumerateNetworkInterfaces(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto type_filter = rp.Pop<u32>();
    // The local SDK command 6 consumes 8-byte entries, then expands them to
    // 12-byte NetworkInterfaceInfo objects: type, MAC[6], is_mac_valid.
    struct SfNetworkInterfaceInfo {
        u8 type{};
        std::array<u8, 6> mac{};
        u8 is_mac_valid{};
    };
    static_assert(sizeof(SfNetworkInterfaceInfo) == 8);
    const size_t capacity = ctx.GetWriteBufferSize() / sizeof(SfNetworkInterfaceInfo);
    std::vector<SfNetworkInterfaceInfo> entries;
    for (const auto& adapter : Network::GetAvailableNetworkInterfaces()) {
        const auto type = static_cast<u8>(adapter.wireless
            ? NetworkInterfaceType::WiFi_Ieee80211 : NetworkInterfaceType::Ethernet);
        if (type_filter != 0 && (type_filter & type) == 0) {
            continue;
        }
        if (entries.size() >= capacity) {
            break;
        }
        entries.push_back({type, adapter.mac_address,
            static_cast<u8>(std::ranges::any_of(adapter.mac_address,
                                               [](u8 byte) { return byte != 0; }))});
    }
    if (!entries.empty()) {
        ctx.WriteBuffer(entries);
    }
    LOG_INFO(Service_NIFM, "Host interfaces: filter={} entries={} capacity={}; addresses omitted",
             type_filter, entries.size(), capacity);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(static_cast<u32>(entries.size()));
}
void IGeneralService::RemoveNetworkProfile(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::GetCurrentIpAddress(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    auto ipv4 = Network::GetHostIPv4Address();
    if (!ipv4) {
        LOG_ERROR(Service_NIFM, "Couldn't get host IPv4 address, defaulting to 0.0.0.0");
        ipv4.emplace(Network::IPv4Address{0, 0, 0, 0});
    }

    // When we're connected to a room, spoof the hosts IP address
    if (auto room_member = network.GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            ipv4 = room_member->GetFakeIpAddress();
        }
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushRaw(*ipv4);
}

void IGeneralService::CreateTemporaryNetworkProfile(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    ASSERT_MSG(ctx.GetReadBufferSize() == 0x17c, "SfNetworkProfileData is not the correct size");
    u128 uuid{};
    auto buffer = ctx.ReadBuffer();
    std::memcpy(&uuid, buffer.data() + 8, sizeof(u128));

    IPC::ResponseBuilder rb{ctx, 6, 0, 1};

    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INetworkProfile>(system);
    rb.PushRaw<u128>(uuid);
}

void IGeneralService::GetCurrentIpConfigInfo(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    struct IpConfigInfo {
        IpAddressSetting ip_address_setting{};
        DnsSetting dns_setting{};
    };
    static_assert(sizeof(IpConfigInfo) == sizeof(IpAddressSetting) + sizeof(DnsSetting),
                  "IpConfigInfo has incorrect size.");

    const auto net_iface = Network::GetSelectedNetworkInterface();

    IpConfigInfo ip_config_info = [&net_iface] {
        if (!net_iface) {
            return IpConfigInfo{};
        }

        return IpConfigInfo{
            .ip_address_setting{
                .is_automatic{true},
                .ip_address{Network::TranslateIPv4(net_iface->ip_address)},
                .subnet_mask{Network::TranslateIPv4(net_iface->subnet_mask)},
                .default_gateway{Network::TranslateIPv4(net_iface->gateway)},
            },
            .dns_setting{
                .is_automatic{true},
                .primary_dns{Network::TranslateIPv4(net_iface->dns_servers[0])},
                .secondary_dns{Network::TranslateIPv4(net_iface->dns_servers[1])},
            },
        };
    }();

    // When we're connected to a room, spoof the hosts IP address
    if (auto room_member = network.GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            ip_config_info.ip_address_setting.ip_address = room_member->GetFakeIpAddress();
        }
    }

    IPC::ResponseBuilder rb{ctx, 2 + (sizeof(IpConfigInfo) + 3) / sizeof(u32)};
    rb.Push(ResultSuccess);
    rb.PushRaw<IpConfigInfo>(ip_config_info);
}

void IGeneralService::EnumerateNetworkProfiles(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto filter = rp.Pop<u8>();
    // nn::nifm::detail::sf::NetworkProfileBasicInfo uses byte alignment.
    struct BasicInfo {
        std::array<u8, 16> uuid{};
        std::array<char, 64> name{};
        u8 profile_type{1};
        u8 connection_type{};
        u8 ssid_length{};
        std::array<char, 32> ssid{};
        u8 authentication{};
        u8 encryption{};
    };
    static_assert(sizeof(BasicInfo) == 0x75);
    const auto iface = Network::GetSelectedNetworkInterface();
    const bool available = iface && (filter & 1) != 0;
    if (available && ctx.GetWriteBufferSize() >= sizeof(BasicInfo)) {
        BasicInfo info{};
        const u64 hash = Common::CityHash64(iface->name.data(), iface->name.size());
        const u128 uuid{hash, hash};
        std::memcpy(info.uuid.data(), &uuid, sizeof(uuid));
        const auto& name = iface->ssid.empty() ? iface->name : iface->ssid;
        std::memcpy(info.name.data(), name.data(), std::min(name.size(), info.name.size() - 1));
        info.connection_type = iface->wireless ? 1 : 2;
        info.ssid_length = static_cast<u8>(std::min(iface->ssid.size(), info.ssid.size()));
        std::memcpy(info.ssid.data(), iface->ssid.data(), info.ssid_length);
        ctx.WriteBuffer(info);
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(available ? 1 : 0);
}

void IGeneralService::ConfirmSystemAvailability(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::SetBackgroundRequestEnabled(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::IsWirelessCommunicationEnabled(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(1);
}

void IGeneralService::GetInternetConnectionStatus(HLERequestContext& ctx) {
    const auto iface = Network::GetSelectedNetworkInterface();
    if (!iface) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Result{ErrorModule::NIFM, 1699});
        return;
    }
    struct Output {
        u8 type;
        u8 wifi_strength;
        InternetConnectionStatus state;
    };
    static_assert(sizeof(Output) == 3);
    const Output out{static_cast<u8>(iface->wireless ? NetworkInterfaceType::WiFi_Ieee80211 : NetworkInterfaceType::Ethernet),
                     iface->wireless ? iface->signal_bars : u8{0}, InternetConnectionStatus::Connected};
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushRaw(out);
}

void IGeneralService::IsEthernetCommunicationEnabled(HLERequestContext& ctx) {
    const auto iface = Network::GetSelectedNetworkInterface();
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(iface && !iface->wireless ? 1 : 0);
}

void IGeneralService::IsAnyInternetRequestAccepted(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(Network::GetHostIPv4Address().has_value() ? 1 : 0);
}

void IGeneralService::IsAnyForegroundRequestAccepted(HLERequestContext& ctx) {
    const bool is_accepted = Network::GetHostIPv4Address().has_value();

    LOG_INFO(Service_NIFM, "called, is_accepted={}", is_accepted);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(is_accepted ? 1 : 0);
}

void IGeneralService::GetSsidListVersion(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    constexpr u32 ssid_list_version = 0;

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(ssid_list_version);
}

void IGeneralService::GetNetworkProfile(HLERequestContext& ctx) {
    GetCurrentNetworkProfile(ctx);
}

void IGeneralService::SetNetworkProfile(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::GetScanData(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::GetCurrentAccessPoint(HLERequestContext& ctx) {
    const auto iface = Network::GetSelectedNetworkInterface();
    if (!iface || !iface->wireless || !iface->wireless_connection_available) {
        LOG_INFO(Service_NIFM, "Current access point unavailable on selected host interface");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultNetworkCommunicationDisabled);
        return;
    }
    SfAccessPointData data{};
    data.ssid_length = static_cast<u8>(std::min(iface->ssid.size(), data.ssid.size()));
    std::copy_n(iface->ssid.begin(), data.ssid_length, data.ssid.begin());
    data.bssid = iface->access_point_mac;
    data.rssi = iface->rssi_dbm;
    data.signal_level = iface->signal_bars;
    data.channel = iface->wireless_channel;
    ctx.WriteBuffer(data);
    LOG_INFO(Service_NIFM,
             "Host access point: frequency_khz={} channel={} signal_level={} auth={} cipher={}; "
             "SSID and addresses omitted",
             iface->frequency_khz, data.channel, data.signal_level,
             iface->wireless_authentication, iface->wireless_cipher);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::Shutdown(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::GetAllowedChannels(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // No channels explicitly allowed (bitmap of allowed channels)
}

void IGeneralService::SetAcceptableNetworkTypeFlag(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::GetAcceptableNetworkTypeFlag(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(3); // Accept both WiFi and Ethernet
}

void IGeneralService::NotifyConnectionStateChanged(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::SetWowlDelayedWakeTime(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::SetWowlTcpKeepAliveTimeout(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::IsWiredConnectionAvailable(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called IsWiredConnectionAvailable [18.0.0+]");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(1); // Wired connection available
}

void IGeneralService::IsNetworkEmulationFeatureEnabled(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called IsNetworkEmulationFeatureEnabled [18.0.0+]");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(0); // Network emulation disabled
}

void IGeneralService::SelectActiveNetworkEmulationProfileIdForDebug(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM,
                "(STUBBED) called SelectActiveNetworkEmulationProfileIdForDebug [18.0.0+]");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::GetScanData2(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called GetScanData [19.0.0+]");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // No scan data
}

void IGeneralService::ResetActiveNetworkEmulationProfileId(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called ResetActiveNetworkEmulationProfileId [20.0.0+]");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::GetActiveNetworkEmulationProfileId(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called GetActiveNetworkEmulationProfileId [18.0.0+]");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // No active profile
}

void IGeneralService::IsRewriteFeatureEnabled(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called IsRewriteFeatureEnabled [18.0.0+]");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(0); // Rewrite feature disabled
}

void IGeneralService::CreateRewriteRule(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called CreateRewriteRule [18.0.0+]");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::DestroyRewriteRule(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called DestroyRewriteRule [18.0.0+]");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::IsActiveNetworkEmulationProfileIdSelected(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM,
                "(STUBBED) called IsActiveNetworkEmulationProfileIdSelected [20.0.0+]");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(0); // No profile selected
}

void IGeneralService::SelectDefaultNetworkEmulationProfileId(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called SelectDefaultNetworkEmulationProfileId [20.0.0+]");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::GetDefaultNetworkEmulationProfileId(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called GetDefaultNetworkEmulationProfileId [20.0.0+]");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Default profile ID
}

void IGeneralService::GetNetworkEmulationProfile(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called GetNetworkEmulationProfile [20.0.0+]");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

IGeneralService::IGeneralService(Core::System& system_)
    : ServiceFramework{system_, "IGeneralService"}, network{system_.GetRoomNetwork()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {1, &IGeneralService::GetClientId, "GetClientId"},
        {2, &IGeneralService::CreateScanRequest, "CreateScanRequest"},
        {4, &IGeneralService::CreateRequest, "CreateRequest"},
        {5, &IGeneralService::GetCurrentNetworkProfile, "GetCurrentNetworkProfile"},
        {6, &IGeneralService::EnumerateNetworkInterfaces, "EnumerateNetworkInterfaces"},
        {7, &IGeneralService::EnumerateNetworkProfiles, "EnumerateNetworkProfiles"},
        {8, &IGeneralService::GetNetworkProfile, "GetNetworkProfile"},
        {9, &IGeneralService::SetNetworkProfile, "SetNetworkProfile"},
        {10, &IGeneralService::RemoveNetworkProfile, "RemoveNetworkProfile"},
        {11, nullptr, "GetScanDataOld"},
        {12, &IGeneralService::GetCurrentIpAddress, "GetCurrentIpAddress"},
        {13, nullptr, "GetCurrentAccessPointOld"},
        {14, &IGeneralService::CreateTemporaryNetworkProfile, "CreateTemporaryNetworkProfile"},
        {15, &IGeneralService::GetCurrentIpConfigInfo, "GetCurrentIpConfigInfo"},
        {16, nullptr, "SetWirelessCommunicationEnabled"},
        {17, &IGeneralService::IsWirelessCommunicationEnabled, "IsWirelessCommunicationEnabled"},
        {18, &IGeneralService::GetInternetConnectionStatus, "GetInternetConnectionStatus"},
        {19, nullptr, "SetEthernetCommunicationEnabled"},
        {20, &IGeneralService::IsEthernetCommunicationEnabled, "IsEthernetCommunicationEnabled"},
        {21, &IGeneralService::IsAnyInternetRequestAccepted, "IsAnyInternetRequestAccepted"},
        {22, &IGeneralService::IsAnyForegroundRequestAccepted, "IsAnyForegroundRequestAccepted"},
        {23, nullptr, "PutToSleep"},
        {24, nullptr, "WakeUp"},
        {25, &IGeneralService::GetSsidListVersion, "GetSsidListVersion"},
        {26, nullptr, "SetExclusiveClient"},
        {27, nullptr, "GetDefaultIpSetting"},
        {28, nullptr, "SetDefaultIpSetting"},
        {29, nullptr, "SetWirelessCommunicationEnabledForTest"},
        {30, nullptr, "SetEthernetCommunicationEnabledForTest"},
        {31, nullptr, "GetTelemetorySystemEventReadableHandle"},
        {32, nullptr, "GetTelemetryInfo"},
        {33, &IGeneralService::ConfirmSystemAvailability, "ConfirmSystemAvailability"},
        {34, &IGeneralService::SetBackgroundRequestEnabled, "SetBackgroundRequestEnabled"},
        {35, &IGeneralService::GetScanData, "GetScanData"},
        {36, &IGeneralService::GetCurrentAccessPoint, "GetCurrentAccessPoint"},
        {37, &IGeneralService::Shutdown, "Shutdown"},
        {38, &IGeneralService::GetAllowedChannels, "GetAllowedChannels"},
        {39, nullptr, "NotifyApplicationSuspended"},
        {40, &IGeneralService::SetAcceptableNetworkTypeFlag, "SetAcceptableNetworkTypeFlag"},
        {41, &IGeneralService::GetAcceptableNetworkTypeFlag, "GetAcceptableNetworkTypeFlag"},
        {42, &IGeneralService::NotifyConnectionStateChanged, "NotifyConnectionStateChanged"},
        {43, &IGeneralService::SetWowlDelayedWakeTime, "SetWowlDelayedWakeTime"},
        {44, &IGeneralService::IsWiredConnectionAvailable, "IsWiredConnectionAvailable"},
        {45, &IGeneralService::IsNetworkEmulationFeatureEnabled, "IsNetworkEmulationFeatureEnabled"},
        {46, &IGeneralService::SelectActiveNetworkEmulationProfileIdForDebug, "SelectActiveNetworkEmulationProfileIdForDebug"},
        {47, &IGeneralService::GetScanData2, "GetScanData"},
        {48, &IGeneralService::ResetActiveNetworkEmulationProfileId, "ResetActiveNetworkEmulationProfileId"},
        {49, &IGeneralService::GetActiveNetworkEmulationProfileId, "GetActiveNetworkEmulationProfileId"},
        {50, &IGeneralService::IsRewriteFeatureEnabled, "IsRewriteFeatureEnabled"},
        {51, &IGeneralService::CreateRewriteRule, "CreateRewriteRule"},
        {52, &IGeneralService::DestroyRewriteRule, "DestroyRewriteRule"},
        {53, &IGeneralService::IsActiveNetworkEmulationProfileIdSelected, "IsActiveNetworkEmulationProfileIdSelected"},
        {54, &IGeneralService::SelectDefaultNetworkEmulationProfileId, "SelectDefaultNetworkEmulationProfileId"},
        {55, &IGeneralService::GetDefaultNetworkEmulationProfileId, "GetDefaultNetworkEmulationProfileId"},
        {56, &IGeneralService::GetNetworkEmulationProfile, "GetNetworkEmulationProfile"},
        {57, &IGeneralService::SetWowlTcpKeepAliveTimeout, "SetWowlTcpKeepAliveTimeout"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IGeneralService::~IGeneralService() = default;

class NetworkInterface final : public ServiceFramework<NetworkInterface> {
public:
    explicit NetworkInterface(const char* name, Core::System& system_)
        : ServiceFramework{system_, name} {
        static const FunctionInfo functions[] = {
            {4, &NetworkInterface::CreateGeneralServiceOld, "CreateGeneralServiceOld"},
            {5, &NetworkInterface::CreateGeneralService, "CreateGeneralService"},
        };
        RegisterHandlers(functions);
    }

private:
    void CreateGeneralServiceOld(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IGeneralService>(system);
    }

    void CreateGeneralService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IGeneralService>(system);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("nifm:a",
                                         std::make_shared<NetworkInterface>("nifm:a", system));
    server_manager->RegisterNamedService("nifm:s",
                                         std::make_shared<NetworkInterface>("nifm:s", system));
    server_manager->RegisterNamedService("nifm:u",
                                         std::make_shared<NetworkInterface>("nifm:u", system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::NIFM
