// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/settings.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/hle/kernel/svc/nextendo_deadline_watch.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/memory.h"

namespace Service::Sockets {

static std::mutex g_last_host_mutex;
static std::unordered_map<std::string, std::string> g_last_host_for_ip;

void SetLastHostForIp(const std::string& ip, const std::string& host) {
    std::lock_guard lock(g_last_host_mutex);
    g_last_host_for_ip[ip] = host;
}

std::string GetLastHostForIp(const std::string& ip) {
    std::lock_guard lock(g_last_host_mutex);
    auto it = g_last_host_for_ip.find(ip);
    if (it != g_last_host_for_ip.end()) {
        return it->second;
    }
    return "";
}

// [Nextendo] See sfdnsres.h's declaration comment. Real bug this exists for (Splatoon 3,
// confirmed via Ryujinx-Nextendo hitting the identical failure on the same guest binary):
// its gRPC channel resolves a redirected Nextendo hostname correctly here, then later loses
// that address in its own connection-establishment plumbing and calls connect() with a
// zeroed IP -- but the SAME port it originally resolved for. Recording "port -> resolved IP"
// at resolution time lets BSD::ConnectImpl recover the real address for that exact port.
static std::mutex g_last_ip_for_port_mutex;
static std::unordered_map<u16, Network::IPv4Address> g_last_ip_for_port;

void SetLastIpForPort(u16 port, Network::IPv4Address ip) {
    if (port == 0) {
        return;
    }
    std::lock_guard lock(g_last_ip_for_port_mutex);
    g_last_ip_for_port[port] = ip;
}

std::optional<Network::IPv4Address> GetLastIpForPort(u16 port) {
    if (port == 0) {
        return std::nullopt;
    }
    std::lock_guard lock(g_last_ip_for_port_mutex);
    auto it = g_last_ip_for_port.find(port);
    if (it != g_last_ip_for_port.end()) {
        return it->second;
    }
    return std::nullopt;
}

// No server address is baked in: unconfigured builds fall back to loopback and redirect nowhere.
static std::string GetConfiguredIp(const std::string& setting, const char* env_var) {
    if (!setting.empty()) {
        return setting;
    }
    if (const char* env = std::getenv(env_var); env && *env) {
        return env;
    }
    return "127.0.0.1";
}

// [Nextendo] La redirection est-elle active ?
//
// Le reglage « enable_nextendo » n'existe QUE dans la facade Qt (src/citron/main.cpp) : la facade
// SDL (citron_cmd) ne le cable nulle part et le reecrit a sa valeur par defaut, false, au
// demarrage. Mesure du 2026-08-25 : lance par citron-cmd, Splatoon 3 a resolu
// « t-dce9377b-lp1.lp1.t.npln.srv.nintendo.net » vers 34.49.112.177 — le VRAI serveur de Nintendo —
// alors que le fichier de configuration portait bien enable_nextendo=true.
//
// On accepte donc aussi une activation par l'environnement, exactement comme GetConfiguredIp le
// fait deja pour les deux adresses. Une valeur vide, « 0 », « false » ou « no » ne l'active pas.
static bool RedirectionNextendoActive() {
    if (Settings::values.enable_nextendo.GetValue()) {
        return true;
    }
    const char* env = std::getenv("NEXTENDO_ENABLE");
    if (env == nullptr || *env == '\0') {
        return false;
    }
    const std::string v = Common::ToLower(env);
    return v != "0" && v != "false" && v != "no" && v != "off";
}

static std::optional<std::string> GetNextendoRedirectIp(const std::string& host) {
    if (!RedirectionNextendoActive()) {
        return std::nullopt;
    }

    const std::string server_ip =
        GetConfiguredIp(Settings::values.nextendo_server_ip.GetValue(), "NEXTENDO_SERVER_IP");
    const std::string nat_ip =
        GetConfiguredIp(Settings::values.nextendo_nat_ip.GetValue(), "NEXTENDO_NAT_IP");

    if (host.starts_with("nncs2-") && host.ends_with(".n.n.srv.nintendo.net")) {
        LOG_INFO(Service, "[Nextendo] Redirecting NAT check host '{}' -> '{}'", host, nat_ip);
        return nat_ip;
    }

    if (host == "nintendo.net" || host.ends_with(".nintendo.net") ||
        host == "nintendo.com" || host.ends_with(".nintendo.com") ||
        host == "nintendowifi.net" || host.ends_with(".nintendowifi.net") ||
        host == "nintendo.co.jp" || host.ends_with(".nintendo.co.jp")) {
        LOG_INFO(Service, "[Nextendo] Redirecting Nintendo host '{}' -> '{}'", host, server_ip);
        return server_ip;
    }

    return std::nullopt;
}

// [Nextendo] Debug-only tap: redirects an "npln" host straight to a local TLS-terminating
// proxy instead of production, for protocol inspection. Independent of enable_nextendo (this
// isn't a Nextendo-server redirect, just a temporary debugging aid) -- unset by default, so it
// never affects a normal run. NEXTENDO_S3_DEBUG_PROXY_IP=<ip> to enable.
static std::optional<std::string> GetNplnDebugProxyIp(const std::string& host) {
    if (Common::ToLower(host).find("npln") == std::string::npos) {
        return std::nullopt;
    }
    const char* env = std::getenv("NEXTENDO_S3_DEBUG_PROXY_IP");
    if (!env || !*env) {
        return std::nullopt;
    }
    LOG_INFO(Service, "[Nextendo] Redirecting npln host '{}' -> debug proxy '{}'", host, env);
    return std::string(env);
}

// [Nextendo] Optional delay before the first "npln" host resolution, against a hypothesized
// startup deadlock. Disabled by default (max_wait_ms=0) -- unconfirmed benefit, and a nonzero
// delay now blocks every other socket IPC call too (bsdsocket is single-threaded).
// NEXTENDO_NPLN_DELAY_MS opts back into a fixed wait if ever needed.
static std::once_flag g_npln_delay_once;

static void MaybeDelayNplnInit(const std::string& host) {
    if (Common::ToLower(host).find("npln") == std::string::npos) {
        return;
    }
    std::call_once(g_npln_delay_once, [] {
        int max_wait_ms = 0;
        if (const char* env = std::getenv("NEXTENDO_NPLN_DELAY_MS"); env && *env) {
            try {
                const int parsed = std::stoi(env);
                if (parsed >= 0) {
                    max_wait_ms = parsed;
                }
            } catch (const std::exception&) {
                // Malformed override -- keep the default rather than fail resolution over it.
            }
        }
        if (max_wait_ms <= 0) {
            return;
        }

        LOG_INFO(Service,
                 "[Nextendo] Holding the first npln host resolution until the JIT/shader-compile "
                 "burst settles before gRPC's connection setup starts ({} ms) (see "
                 "MaybeDelayNplnInit)",
                 max_wait_ms);

        std::this_thread::sleep_for(std::chrono::milliseconds(max_wait_ms));

        LOG_INFO(Service, "[Nextendo] npln hold finished after {} ms", max_wait_ms);

        // [Nextendo][DIAG] Arm a short window during which any finite, non-trivial
        // WaitSynchronization timeout gets logged -- see nextendo_deadline_watch.h. This is
        // trying to directly OBSERVE the game's gRPC call deadline (if it's implemented as a
        // timed kernel wait) rather than continuing to guess at it via static binary analysis.
        Kernel::Svc::ArmNextendoDeadlineWatch(90000);
        LOG_INFO(Service, "[Nextendo][DIAG] Deadline watch armed for 90000 ms");
    });
}

enum class NetDbError : s32 {
    Internal = -1,
    Success = 0,
    HostNotFound = 1,
    TryAgain = 2,
    NoRecovery = 3,
    NoData = 4,
};

SFDNSRES::SFDNSRES(Core::System& system_) : ServiceFramework{system_, "sfdnsres"} {
    static const FunctionInfo functions[] = {
        {0, &SFDNSRES::SetDnsAddresses, "SetDnsAddressesPrivateRequest"},
        {1, &SFDNSRES::GetDnsAddressList, "GetDnsAddressPrivateRequest"},
        {2, &SFDNSRES::GetHostByNameRequest, "GetHostByNameRequest"},
        {3, &SFDNSRES::GetHostByAddrRequest, "GetHostByAddrRequest"},
        {4, &SFDNSRES::GetHostStringError, "GetHostStringErrorRequest"},
        {5, &SFDNSRES::GetGaiStringErrorRequest, "GetGaiStringErrorRequest"},
        {6, &SFDNSRES::GetAddrInfoRequest, "GetAddrInfoRequest"},
        {7, &SFDNSRES::GetNameInfoRequest, "GetNameInfoRequest"},
        {8, &SFDNSRES::GetCancelHandleRequest, "GetCancelHandleRequest"},
        {9, &SFDNSRES::CancelRequest, "CancelRequest"},
        {10, &SFDNSRES::GetHostByNameRequestWithOptions, "GetHostByNameRequestWithOptions"},
        {11, &SFDNSRES::GetHostByAddrRequest, "GetHostByAddrRequestWithOptions"},
        {12, &SFDNSRES::GetAddrInfoRequestWithOptions, "GetAddrInfoRequestWithOptions"},
        {13, &SFDNSRES::GetNameInfoRequestWithOptions, "GetNameInfoRequestWithOptions"},
        {14, &SFDNSRES::ResolverSetOptionRequest, "ResolverSetOptionRequest"},
        {15, &SFDNSRES::GetOptions, "ResolverGetOptionRequest"},
    };
    RegisterHandlers(functions);
}

SFDNSRES::~SFDNSRES() = default;

static NetDbError GetAddrInfoErrorToNetDbError(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        return NetDbError::Success;
    case GetAddrInfoError::AGAIN:
        return NetDbError::TryAgain;
    case GetAddrInfoError::NODATA:
        return NetDbError::HostNotFound;
    case GetAddrInfoError::SERVICE:
        return NetDbError::Success;
    default:
        return NetDbError::HostNotFound;
    }
}

static Errno GetAddrInfoErrorToErrno(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        return Errno::SUCCESS;
    case GetAddrInfoError::AGAIN:
        return Errno::SUCCESS;
    case GetAddrInfoError::NODATA:
        return Errno::SUCCESS;
    case GetAddrInfoError::SERVICE:
        return Errno::INVAL;
    default:
        return Errno::SUCCESS;
    }
}

template <typename T>
static void Append(std::vector<u8>& vec, T t) {
    const size_t offset = vec.size();
    vec.resize(offset + sizeof(T));
    std::memcpy(vec.data() + offset, &t, sizeof(T));
}

static void AppendNulTerminated(std::vector<u8>& vec, std::string_view str) {
    const size_t offset = vec.size();
    vec.resize(offset + str.size() + 1);
    std::memmove(vec.data() + offset, str.data(), str.size());
}

// We implement gethostbyname using the host's getaddrinfo rather than the
// host's gethostbyname, because it simplifies portability: e.g., getaddrinfo
// behaves the same on Unix and Windows, unlike gethostbyname where Windows
// doesn't implement h_errno.
static std::vector<u8> SerializeAddrInfoAsHostEnt(const std::vector<Network::AddrInfo>& vec,
                                                  std::string_view host) {

    std::vector<u8> data;
    // h_name: use the input hostname (append nul-terminated)
    AppendNulTerminated(data, host);
    // h_aliases: leave empty

    Append<u32_be>(data, 0); // count of h_aliases
    // (If the count were nonzero, the aliases would be appended as nul-terminated here.)
    Append<u16_be>(data, static_cast<u16>(Domain::INET)); // h_addrtype
    Append<u16_be>(data, sizeof(Network::IPv4Address));   // h_length
    // h_addr_list:
    size_t count = vec.size();
    ASSERT(count <= UINT32_MAX);
    Append<u32_be>(data, static_cast<uint32_t>(count));
    for (const Network::AddrInfo& addrinfo : vec) {
        // On the Switch, this is passed through htonl despite already being
        // big-endian, so it ends up as little-endian.
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip));

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }
    return data;
}

std::set<std::string> blocked_domains{
    // stupid hogwarts
    "phoenix-api.wbagora.com",
    // prevents various battle net games from crashing
    "battle.net",
    // minecraft from crashing
    "microsoft.com",
    "mojang.com",
    "xboxlive.com",
    "minecraftservices.com",
};

static std::pair<u32, GetAddrInfoError> GetHostByNameRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_WARNING(
        Service,
        "called with ignored parameters: use_nsd_resolve={}, cancel_handle={}, process_id={}",
        parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);

    LOG_INFO(Service, "[Nextendo] DNS resolve (GetHostByName) requested: host={}", host);

    // [Nextendo] See MaybeDelayNplnInit's declaration comment.
    MaybeDelayNplnInit(host);

    if (parameters.use_nsd_resolve || host.find('%') != std::string::npos) {
        auto pos = host.find('%');
        if (pos != std::string::npos) {
            host.replace(pos, 1, "lp1");
        }
        if (host == "api.accounts.nintendo.com" || host == "accounts.nintendo.com") {
            host = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
        }
        LOG_INFO(Service, "[sfdnsres] NSD resolved host to '{}'", host);
    }

    std::string query_host = host;
    auto redirect = GetNextendoRedirectIp(host);
    if (redirect.has_value()) {
        query_host = *redirect;
    } else if (blocked_domains.find(host) != blocked_domains.end()) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    auto res = Network::GetAddressInfo(query_host, /*service*/ std::nullopt);
    if (!res.has_value()) {
        return {0, Translate(res.error())};
    }

    if (redirect.has_value()) {
        for (const auto& addrinfo : res.value()) {
            SetLastHostForIp(Network::IPv4AddressToString(addrinfo.addr.ip), host);
        }
    }

    const std::vector<u8> data = SerializeAddrInfoAsHostEnt(res.value(), host);
    const u32 data_size = static_cast<u32>(data.size());
    ctx.WriteBuffer(data, 0);

    return {data_size, GetAddrInfoError::SUCCESS};
}

void SFDNSRES::GetHostByNameRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        NetDbError netdb_error;
        Errno bsd_errno;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .data_size = data_size,
    });
}

void SFDNSRES::GetHostByNameRequestWithOptions(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

static std::vector<u8> SerializeAddrInfo(const std::vector<Network::AddrInfo>& vec,
                                         std::string_view host) {
    // Adapted from
    // https://github.com/switchbrew/libnx/blob/c5a9a909a91657a9818a3b7e18c9b91ff0cbb6e3/nx/source/runtime/resolver.c#L190
    std::vector<u8> data;

    for (const Network::AddrInfo& addrinfo : vec) {
        // serialized addrinfo:
        Append<u32_be>(data, 0xBEEFCAFE);                                        // magic
        Append<u32_be>(data, 0);                                                 // ai_flags
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.family)));      // ai_family
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.socket_type))); // ai_socktype
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.protocol)));    // ai_protocol
        Append<u32_be>(data, sizeof(SockAddrIn));                                // ai_addrlen
        // ^ *not* sizeof(SerializedSockAddrIn), not that it matters since they're the same size

        // ai_addr: BSD-style sockaddr_in, matching the SockAddrIn struct in sockets.h --
        // {u8 sin_len; u8 sin_family; u16 sin_port; u8 sin_addr[4]; u8 sin_zero[8];}. This used
        // to write sin_family as a single 2-byte big-endian value, which skips sin_len entirely
        // (leaving it implicitly 0x00) instead of emitting it as its own leading byte. A guest
        // resolver walker that trusts sin_len -- and gRPC-based titles (Splatoon 3) that build
        // their own connect() sockaddr straight out of this buffer -- reads a zero-length
        // address off a sin_len of 0 and falls back to connecting to 0.0.0.0. Exactly matches
        // Ryujinx-Nextendo's AddrInfo4.Length fix (was sizeof(Array4<byte>)=4, needed to be
        // sizeof(AddrInfo4)=16): sin_len must be the full sockaddr size, not folded away.
        // [Nextendo]
        Append<u8>(data, static_cast<u8>(sizeof(SockAddrIn)));              // sin_len
        Append<u8>(data, static_cast<u8>(Translate(addrinfo.addr.family))); // sin_family
        // On the Switch, the following fields are passed through htonl despite
        // already being big-endian, so they end up as little-endian.
        Append<u16_le>(data, addrinfo.addr.portno);                            // sin_port
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip)); // sin_addr
        data.resize(data.size() + 8, 0);                                       // sin_zero

        if (addrinfo.canon_name.has_value()) {
            AppendNulTerminated(data, *addrinfo.canon_name);
        } else {
            data.push_back(0);
        }

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }

    data.resize(data.size() + 4, 0); // 4-byte sentinel value

    return data;
}

static std::pair<u32, GetAddrInfoError> GetAddrInfoRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_WARNING(
        Service,
        "called with ignored parameters: use_nsd_resolve={}, cancel_handle={}, process_id={}",
        parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);

    LOG_INFO(Service, "[Nextendo] DNS resolve (GetAddrInfo) requested: host={}", host);

    // [Nextendo] See MaybeDelayNplnInit's declaration comment.
    MaybeDelayNplnInit(host);

    // [Nextendo] A literal IP has nothing to resolve -- return it as-is, before any of the
    // redirect/blocklist/NSD-rewrite logic below, all of which exist to turn a HOSTNAME into
    // the right address and have no business touching an address that's already one. See
    // TryParseIPv4Literal's declaration comment in internal_network/network.h for why falling
    // through to a real resolution here (which is what happened before this check existed) was
    // the actual root cause of Splatoon 3's NPLN connections completing TCP+TLS+HTTP/2 and then
    // silently closing without ever sending a HEADERS frame -- confirmed live: citron's own
    // connect cycles for this exact hostname resolved and connected correctly, but every one
    // still sent only a single small request and closed within ~1s of the server's reply, on
    // every cycle regardless of timing -- consistent with the game building an HTTP/2
    // :authority header from a corrupted canonical name, not any citron-side socket/scheduling
    // issue (both were separately investigated at length and ruled out).
    if (Network::IPv4Address literal_ip; Network::TryParseIPv4Literal(host, literal_ip)) {
        LOG_DEBUG(Service, "[Nextendo] Host '{}' is already a literal address: returned as-is",
                  host);
        Network::AddrInfo entry{};
        entry.family = Network::Domain::INET;
        entry.socket_type = Network::Type::STREAM;
        entry.protocol = Network::Protocol::TCP;
        entry.addr.family = Network::Domain::INET;
        entry.addr.ip = literal_ip;
        entry.addr.portno = 0;
        entry.canon_name = host;

        // Deliberately no SetLastHostForIp here, matching Ryujinx-Nextendo's own fix -- a
        // literal IP carries no hostname to record, and recording one would corrupt the
        // reverse lookup table used elsewhere for this exact purpose (see
        // GetLastHostForIp's declaration comment).
        const std::vector<u8> data = SerializeAddrInfo({entry}, host);
        const u32 data_size = static_cast<u32>(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }

    if (parameters.use_nsd_resolve || host.find('%') != std::string::npos) {
        auto pos = host.find('%');
        if (pos != std::string::npos) {
            host.replace(pos, 1, "lp1");
        }
        if (host == "api.accounts.nintendo.com" || host == "accounts.nintendo.com") {
            host = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
        }
        LOG_INFO(Service, "[sfdnsres] NSD resolved host to '{}'", host);
    }

    std::string query_host = host;
    auto redirect = GetNplnDebugProxyIp(host);
    if (!redirect.has_value()) {
        redirect = GetNextendoRedirectIp(host);
    }
    if (redirect.has_value()) {
        query_host = *redirect;
    } else if (blocked_domains.find(host) != blocked_domains.end()) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    std::optional<std::string> service = std::nullopt;
    if (ctx.CanReadBuffer(1)) {
        const std::span<const u8> service_buffer = ctx.ReadBuffer(1);
        service = Common::StringFromBuffer(service_buffer);
    }

    auto res = Network::GetAddressInfo(query_host, service);
    if (!res.has_value()) {
        return {0, Translate(res.error())};
    }

    if (redirect.has_value()) {
        // [Nextendo] GetAddressInfo never requests AI_CANONNAME, so canon_name comes back
        // unset for a normal resolution -- SerializeAddrInfo then writes an empty canonical
        // name. That's harmless for titles that never look at it, but this query just resolved
        // a REDIRECT TARGET (a literal IP string), not the real host, so even if canon_name
        // were populated by the OS resolver it would be the numeric IP, never the actual
        // hostname. A gRPC-based title (Splatoon 3) builds its HTTP/2 :authority header from
        // this canonical name -- sending it empty (or the wrong literal IP) instead of the real
        // npln hostname is exactly the shape of bug already fixed above for an already-literal
        // host (see that comment): TCP+TLS+HTTP/2 complete fine, then the connection is torn
        // down before a real HEADERS frame ever goes out. Force it back to the real host here,
        // matching that same fix.
        for (auto& addrinfo : res.value()) {
            addrinfo.canon_name = host;
        }

        // [Nextendo] Port-keyed recovery for gRPC-based titles (Splatoon 3) that lose this
        // resolved address later. See SetLastIpForPort's declaration comment in sfdnsres.h.
        std::optional<u16> service_port;
        if (service.has_value()) {
            try {
                const int parsed = std::stoi(*service);
                if (parsed > 0 && parsed <= 0xFFFF) {
                    service_port = static_cast<u16>(parsed);
                }
            } catch (const std::exception&) {
                // service wasn't a plain port number (a named service like "http") -- nothing
                // to key the fallback on, and that's fine, most titles never need it anyway.
            }
        }
        for (const auto& addrinfo : res.value()) {
            SetLastHostForIp(Network::IPv4AddressToString(addrinfo.addr.ip), host);
            if (service_port.has_value()) {
                SetLastIpForPort(*service_port, addrinfo.addr.ip);
            }
        }
    }

    const std::vector<u8> data = SerializeAddrInfo(res.value(), host);
    const u32 data_size = static_cast<u32>(data.size());
    ctx.WriteBuffer(data, 0);

    return {data_size, GetAddrInfoError::SUCCESS};
}

void SFDNSRES::GetAddrInfoRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        Errno bsd_errno;
        GetAddrInfoError gai_error;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .gai_error = emu_gai_err,
        .data_size = data_size,
    });
}

void SFDNSRES::GetGaiStringErrorRequest(HLERequestContext& ctx) {
    struct InputParameters {
        GetAddrInfoError gai_errno;
    };
    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    const std::string result = Translate(input.gai_errno);
    ctx.WriteBuffer(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetAddrInfoRequestWithOptions(HLERequestContext& ctx) {
    // Additional options are ignored
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        GetAddrInfoError gai_error;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x10);

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .gai_error = emu_gai_err,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

void SFDNSRES::ResolverSetOptionRequest(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    [[maybe_unused]] const u32 option_name = rp.Pop<u32>();
    // Option value is in a buffer
    [[maybe_unused]] const auto option_value_buffer = ctx.ReadBuffer(0);

    LOG_WARNING(Service, "(STUBBED) sfdnsres::ResolverSetOptionRequest called. Option: {}, Value Size: {}", option_name, option_value_buffer.size());

    // Default success for stub
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

// New Stub Implementations
void SFDNSRES::SetDnsAddresses(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::SetDnsAddresses called");
    // Takes input buffer of SockAddrIn. No direct output apart from Result.
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetDnsAddressList(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetDnsAddressList called");
    // Writes SockAddrIn list to output buffer.
    // Returns u32 count, Errno bsd_errno.
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Count
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void SFDNSRES::GetHostByAddrRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetHostByAddrRequest called (deprecated)");
    // Similar return to GetHostByName: NetDbError, Errno, data_size
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushEnum(NetDbError::Internal);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
    rb.Push<u32>(0); // data_size
}

void SFDNSRES::GetHostStringError(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetHostStringError called");
    // Similar to GetGaiStringError: takes error code, returns string in buffer.
    // Returns u32 data_size.
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // data_size
}

void SFDNSRES::GetCancelHandleRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetCancelHandleRequest called");
    // GetCancelHandleRequest(u64 pid_placeholder, pid) -> u32 handle
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0);
}

void SFDNSRES::CancelRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::CancelRequest called");
    // Takes handle. Returns Result.
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetOptions(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetOptions called");
    // Takes option name. Returns option value (u32/buffer?), Errno.
    IPC::ResponseBuilder rb{ctx, 4}; // Result, value (u32 placeholder), errno
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Placeholder for option value
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void SFDNSRES::GetAddrInfoRequestRaw(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetAddrInfoRequestRaw called");
    // Similar to GetAddrInfoRequest: Errno, GetAddrInfoError, data_size
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
    rb.PushEnum(GetAddrInfoError::AGAIN); // Changed from INTERNAL to AGAIN
    rb.Push<u32>(0); // data_size
}

// Stubs for functions from original registration table not in Switchbrew sfdnsres
void SFDNSRES::GetNameInfoRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetNameInfoRequest called");
    IPC::ResponseBuilder rb{ctx, 5}; // Similar to GetAddrInfoRequest
    rb.Push(ResultSuccess);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
    rb.PushEnum(GetAddrInfoError::AGAIN); // Changed from INTERNAL to AGAIN
    rb.Push<u32>(0);
}

void SFDNSRES::GetNameInfoRequestWithOptions(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetNameInfoRequestWithOptions called");
    IPC::ResponseBuilder rb{ctx, 6}; // Similar to GetAddrInfoRequestWithOptions
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // data_size
    rb.PushEnum(GetAddrInfoError::AGAIN); // Changed from INTERNAL to AGAIN
    rb.PushEnum(NetDbError::Internal);    // This should be fine as NetDbError::Internal is defined
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

} // namespace Service::Sockets
