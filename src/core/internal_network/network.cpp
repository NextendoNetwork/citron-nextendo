// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/error.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#else
#error "Unimplemented platform"
#endif

#include "common/assert.h"
#include "common/common_types.h"
#include "common/expected.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"

namespace Network {

namespace {

enum class CallType {
    Send,
    Connect,
    Other,
};

#ifdef _WIN32

using socklen_t = int;

SOCKET interrupt_socket = static_cast<SOCKET>(-1);

void InterruptSocketOperations() {
    closesocket(interrupt_socket);
}

void AcknowledgeInterrupt() {
    interrupt_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

void Initialize() {
    WSADATA wsa_data;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa_data);

    AcknowledgeInterrupt();
}

void Finalize() {
    InterruptSocketOperations();
    WSACleanup();
}

SOCKET GetInterruptSocket() {
    return interrupt_socket;
}

sockaddr TranslateFromSockAddrIn(SockAddrIn input) {
    sockaddr_in result;

#if defined(__unix__) || defined(__APPLE__)
    result.sin_len = sizeof(result);
#endif

    switch (static_cast<Domain>(input.family)) {
    case Domain::INET:
        result.sin_family = AF_INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.family);
        result.sin_family = AF_INET;
        break;
    }

    // input.portno is already in host byte order in our abstraction. Ensure network order here.
    result.sin_port = htons(static_cast<uint16_t>(input.portno));

    auto& ip = result.sin_addr.S_un.S_un_b;
    ip.s_b1 = input.ip[0];
    ip.s_b2 = input.ip[1];
    ip.s_b3 = input.ip[2];
    ip.s_b4 = input.ip[3];

    sockaddr addr;
    std::memcpy(&addr, &result, sizeof(addr));
    return addr;
}

LINGER MakeLinger(bool enable, u32 linger_value) {
    ASSERT(linger_value <= std::numeric_limits<u_short>::max());

    LINGER value;
    value.l_onoff = enable ? 1 : 0;
    value.l_linger = static_cast<u_short>(linger_value);
    return value;
}

bool EnableNonBlock(SOCKET fd, bool enable) {
    u_long value = enable ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &value) != SOCKET_ERROR;
}

Errno TranslateNativeError(int e, CallType call_type = CallType::Other) {
    switch (e) {
    case 0:
        return Errno::SUCCESS;
    case WSAEBADF:
        return Errno::BADF;
    case WSAEINVAL:
        return Errno::INVAL;
    case WSAEMFILE:
        return Errno::MFILE;
    case WSAENOTCONN:
        return Errno::NOTCONN;
    case WSAEWOULDBLOCK:
        // [Nextendo] Windows overloads WSAEWOULDBLOCK for a non-blocking connect() that's
        // actually in progress -- POSIX has a distinct EINPROGRESS for that specific case, and
        // everywhere that's not a connect() call, WSAEWOULDBLOCK really does just mean "would
        // block, try again" (EAGAIN). Returning EAGAIN here for connect() told the guest its
        // connection attempt just plain failed, when the real answer (matching what Unix's
        // errno already unambiguously reports) was "still connecting, come back to it" --
        // confirmed directly: a real connect() to a live server was rejected outright at the
        // very first EAGAIN instead of proceeding through completion via poll(), and the game's
        // own retry loop kept starting the whole sequence over from scratch forever, unable to
        // ever get far enough to actually finish connecting.
        return call_type == CallType::Connect ? Errno::INPROGRESS : Errno::AGAIN;
    case WSAECONNREFUSED:
        return Errno::CONNREFUSED;
    case WSAECONNABORTED:
        if (call_type == CallType::Send) {
            // Winsock yields WSAECONNABORTED from `send` in situations where Unix
            // systems, and actual Switches, yield EPIPE.
            return Errno::PIPE;
        } else {
            return Errno::CONNABORTED;
        }
    case WSAECONNRESET:
        return Errno::CONNRESET;
    case WSAEHOSTUNREACH:
        return Errno::HOSTUNREACH;
    case WSAENETDOWN:
        return Errno::NETDOWN;
    case WSAENETUNREACH:
        return Errno::NETUNREACH;
    case WSAEMSGSIZE:
        return Errno::MSGSIZE;
#ifdef WSAEDESTADDRREQ
    case WSAEDESTADDRREQ:
        return Errno::DESTADDRREQ;
#endif
    case WSAETIMEDOUT:
        return Errno::TIMEDOUT;
    case WSAEINPROGRESS:
        return Errno::INPROGRESS;
    case WSAENOPROTOOPT:
        return Errno::INVAL;
    default:
        UNIMPLEMENTED_MSG("Unimplemented errno={}", e);
        return Errno::OTHER;
    }
}

#elif defined(__unix__) || defined(__APPLE__) // ^ _WIN32 v (defined(__unix__) || defined(__APPLE__))

using SOCKET = int;
using WSAPOLLFD = pollfd;
using ULONG = u64;

constexpr SOCKET SOCKET_ERROR = -1;

constexpr int SD_RECEIVE = SHUT_RD;
constexpr int SD_SEND = SHUT_WR;
constexpr int SD_BOTH = SHUT_RDWR;

int interrupt_pipe_fd[2] = {-1, -1};

void Initialize() {
    if (pipe(interrupt_pipe_fd) != 0) {
        LOG_ERROR(Network, "Failed to create interrupt pipe!");
    }
    int flags = fcntl(interrupt_pipe_fd[0], F_GETFL);
    ASSERT_MSG(fcntl(interrupt_pipe_fd[0], F_SETFL, flags | O_NONBLOCK) == 0,
               "Failed to set nonblocking state for interrupt pipe");
}

void Finalize() {
    if (interrupt_pipe_fd[0] >= 0) {
        close(interrupt_pipe_fd[0]);
    }
    if (interrupt_pipe_fd[1] >= 0) {
        close(interrupt_pipe_fd[1]);
    }
}

void InterruptSocketOperations() {
    u8 value = 0;
    ASSERT(write(interrupt_pipe_fd[1], &value, sizeof(value)) == 1);
}

void AcknowledgeInterrupt() {
    u8 value = 0;
    ssize_t ret = read(interrupt_pipe_fd[0], &value, sizeof(value));
    if (ret != 1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR(Network, "Failed to acknowledge interrupt on shutdown");
    }
}

SOCKET GetInterruptSocket() {
    return interrupt_pipe_fd[0];
}

sockaddr TranslateFromSockAddrIn(SockAddrIn input) {
    sockaddr_in result;

    switch (static_cast<Domain>(input.family)) {
    case Domain::INET:
        result.sin_family = AF_INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.family);
        result.sin_family = AF_INET;
        break;
    }

    result.sin_port = htons(input.portno);

    result.sin_addr.s_addr = input.ip[0] | input.ip[1] << 8 | input.ip[2] << 16 | input.ip[3] << 24;

    sockaddr addr;
    std::memcpy(&addr, &result, sizeof(addr));
    return addr;
}

int WSAPoll(WSAPOLLFD* fds, ULONG nfds, int timeout) {
    return poll(fds, static_cast<nfds_t>(nfds), timeout);
}

int closesocket(SOCKET fd) {
    return close(fd);
}

linger MakeLinger(bool enable, u32 linger_value) {
    linger value;
    value.l_onoff = enable ? 1 : 0;
    value.l_linger = linger_value;
    return value;
}

bool EnableNonBlock(int fd, bool enable) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        return false;
    }
    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags) == 0;
}

Errno TranslateNativeError(int e, CallType call_type = CallType::Other) {
    switch (e) {
    case 0:
        return Errno::SUCCESS;
    case EBADF:
        return Errno::BADF;
    case EINVAL:
        return Errno::INVAL;
    case EMFILE:
        return Errno::MFILE;
    case EPIPE:
        return Errno::PIPE;
    case ECONNABORTED:
        return Errno::CONNABORTED;
    case ENOTCONN:
        return Errno::NOTCONN;
    case EAGAIN:
        return Errno::AGAIN;
    case ECONNREFUSED:
        return Errno::CONNREFUSED;
    case ECONNRESET:
        return Errno::CONNRESET;
    case EHOSTUNREACH:
        return Errno::HOSTUNREACH;
    case ENETDOWN:
        return Errno::NETDOWN;
    case ENETUNREACH:
        return Errno::NETUNREACH;
    case EMSGSIZE:
        return Errno::MSGSIZE;
#ifdef EDESTADDRREQ
    case EDESTADDRREQ:
        return Errno::DESTADDRREQ;
#endif
    case ETIMEDOUT:
        return Errno::TIMEDOUT;
    case EINPROGRESS:
        return Errno::INPROGRESS;
    case ENOPROTOOPT:
        return Errno::INVAL;
    default:
        UNIMPLEMENTED_MSG("Unimplemented errno={} ({})", e, strerror(e));
        return Errno::OTHER;
    }
}

#endif

Errno GetAndLogLastError(CallType call_type = CallType::Other) {
#ifdef _WIN32
    int e = WSAGetLastError();
#else
    int e = errno;
#endif
    const Errno err = TranslateNativeError(e, call_type);
    if (err == Errno::AGAIN || err == Errno::TIMEDOUT || err == Errno::INPROGRESS) {
        // These happen during normal operation, so only log them at debug level.
        LOG_DEBUG(Network, "Socket operation error: {}", Common::NativeErrorToString(e));
        return err;
    }
    LOG_ERROR(Network, "Socket operation error: {}", Common::NativeErrorToString(e));
    return err;
}

GetAddrInfoError TranslateGetAddrInfoErrorFromNative(int gai_err) {
    switch (gai_err) {
    case 0:
        return GetAddrInfoError::SUCCESS;
#ifdef EAI_ADDRFAMILY
    case EAI_ADDRFAMILY:
        return GetAddrInfoError::ADDRFAMILY;
#endif
    case EAI_AGAIN:
        return GetAddrInfoError::AGAIN;
    case EAI_BADFLAGS:
        return GetAddrInfoError::BADFLAGS;
    case EAI_FAIL:
        return GetAddrInfoError::FAIL;
    case EAI_FAMILY:
        return GetAddrInfoError::FAMILY;
    case EAI_MEMORY:
        return GetAddrInfoError::MEMORY;
    case EAI_NONAME:
        return GetAddrInfoError::NONAME;
    case EAI_SERVICE:
        return GetAddrInfoError::SERVICE;
    case EAI_SOCKTYPE:
        return GetAddrInfoError::SOCKTYPE;
        // These codes may not be defined on all systems:
#ifdef EAI_SYSTEM
    case EAI_SYSTEM:
        return GetAddrInfoError::SYSTEM;
#endif
#ifdef EAI_BADHINTS
    case EAI_BADHINTS:
        return GetAddrInfoError::BADHINTS;
#endif
#ifdef EAI_PROTOCOL
    case EAI_PROTOCOL:
        return GetAddrInfoError::PROTOCOL;
#endif
#ifdef EAI_OVERFLOW
    case EAI_OVERFLOW:
        return GetAddrInfoError::OVERFLOW_;
#endif
    default:
#ifdef EAI_NODATA
        // This can't be a case statement because it would create a duplicate
        // case on Windows where EAI_NODATA is an alias for EAI_NONAME.
        if (gai_err == EAI_NODATA) {
            return GetAddrInfoError::NODATA;
        }
#endif
        return GetAddrInfoError::OTHER;
    }
}

Domain TranslateDomainFromNative(int domain) {
    switch (domain) {
    case 0:
        return Domain::Unspecified;
    case AF_INET:
        return Domain::INET;
#ifdef AF_INET6
    case AF_INET6:
        return Domain::INET6;
#endif
    default:
        UNIMPLEMENTED_MSG("Unhandled domain={}", domain);
        return Domain::INET;
    }
}

int TranslateDomainToNative(Domain domain) {
    switch (domain) {
    case Domain::Unspecified:
        return 0;
    case Domain::INET:
        return AF_INET;
    case Domain::INET6:
        return AF_INET6;
    default:
        UNIMPLEMENTED_MSG("Unimplemented domain={}", domain);
        return 0;
    }
}

Type TranslateTypeFromNative(int type) {
    switch (type) {
    case 0:
        return Type::Unspecified;
    case SOCK_STREAM:
        return Type::STREAM;
    case SOCK_DGRAM:
        return Type::DGRAM;
    case SOCK_RAW:
        return Type::RAW;
    case SOCK_SEQPACKET:
        return Type::SEQPACKET;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return Type::STREAM;
    }
}

int TranslateTypeToNative(Type type) {
    switch (type) {
    case Type::Unspecified:
        return 0;
    case Type::STREAM:
        return SOCK_STREAM;
    case Type::DGRAM:
        return SOCK_DGRAM;
    case Type::RAW:
        return SOCK_RAW;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return 0;
    }
}

Protocol TranslateProtocolFromNative(int protocol) {
    switch (protocol) {
    case 0:
        return Protocol::Unspecified;
    case IPPROTO_TCP:
        return Protocol::TCP;
    case IPPROTO_UDP:
        return Protocol::UDP;
    default:
        UNIMPLEMENTED_MSG("Unimplemented protocol={}", protocol);
        return Protocol::Unspecified;
    }
}

int TranslateProtocolToNative(Protocol protocol) {
    switch (protocol) {
    case Protocol::Unspecified:
        return 0;
    case Protocol::TCP:
        return IPPROTO_TCP;
    case Protocol::UDP:
        return IPPROTO_UDP;
    default:
        UNIMPLEMENTED_MSG("Unimplemented protocol={}", protocol);
        return 0;
    }
}

SockAddrIn TranslateToSockAddrIn(sockaddr_in input, size_t input_len) {
    SockAddrIn result;

    result.family = TranslateDomainFromNative(input.sin_family);

    result.portno = ntohs(input.sin_port);

    result.ip = TranslateIPv4(input.sin_addr);

    return result;
}

short TranslatePollEvents(PollEvents events) {
    short result = 0;

    const auto translate = [&result, &events](PollEvents guest, short host) {
        if (True(events & guest)) {
            events &= ~guest;
            result |= host;
        }
    };

    translate(PollEvents::In, POLLIN);
    translate(PollEvents::Pri, POLLPRI);
    translate(PollEvents::Out, POLLOUT);
    translate(PollEvents::Err, POLLERR);
    translate(PollEvents::Hup, POLLHUP);
    translate(PollEvents::Nval, POLLNVAL);
    translate(PollEvents::RdNorm, POLLRDNORM);
    translate(PollEvents::RdBand, POLLRDBAND);
    translate(PollEvents::WrBand, POLLWRBAND);

#ifdef _WIN32
    short allowed_events = POLLRDBAND | POLLRDNORM | POLLWRNORM;
    // Unlike poll on other OSes, WSAPoll will complain if any other flags are set on input.
    if (result & ~allowed_events) {
        LOG_DEBUG(Network,
                  "Removing WSAPoll input events 0x{:x} because Windows doesn't support them",
                  result & ~allowed_events);
    }
    result &= allowed_events;
#endif

    UNIMPLEMENTED_IF_MSG((u16)events != 0, "Unhandled guest events=0x{:x}", (u16)events);

    return result;
}

PollEvents TranslatePollRevents(short revents) {
    PollEvents result{};
    const auto translate = [&result, &revents](short host, PollEvents guest) {
        if ((revents & host) != 0) {
            revents &= static_cast<short>(~host);
            result |= guest;
        }
    };

    translate(POLLIN, PollEvents::In);
    translate(POLLPRI, PollEvents::Pri);
    translate(POLLOUT, PollEvents::Out);
    translate(POLLERR, PollEvents::Err);
    translate(POLLHUP, PollEvents::Hup);
    translate(POLLNVAL, PollEvents::Nval);
    translate(POLLRDNORM, PollEvents::RdNorm);
    translate(POLLRDBAND, PollEvents::RdBand);
    translate(POLLWRBAND, PollEvents::WrBand);

    UNIMPLEMENTED_IF_MSG(revents != 0, "Unhandled host revents=0x{:x}", revents);

    return result;
}

// [Nextendo] WSAPoll never reports POLLHUP/POLLERR for a peer that closed or reset an
// already-established TCP connection -- per MSDN, the exceptfds-derived bits it sets only
// cover OOB data and a failed connect(), never a FIN/RST on a live socket. A socket whose peer
// hung up therefore polls as revents=0 forever: to the guest that reads as "nothing ready yet"
// indefinitely instead of "this connection is dead", starving whatever retry/give-up logic
// depends on ever actually seeing an error. Ryujinx-Nextendo hit and fixed this same Windows
// gap (see ManagedSocketPollManager.cs's IsPeerHungUp -- measured 142 consecutive empty polls,
// 38.8s, stuck on a gRPC socket before that fix landed). Peek a byte directly for the real
// signal instead of trusting WSAPoll's revents alone: 0 back means an orderly close (FIN
// already received), a reset-family error means a hard hangup, and anything else (in
// particular WSAEWOULDBLOCK) means truly nothing yet -- left alone.
//
// WIN32-only: this whole gap is specific to WSAPoll's exceptfds behavior. Real POSIX poll()
// on Linux/macOS already reports POLLHUP/POLLERR correctly for a hung-up peer, and this
// function's SOCKET/SOCKET_ERROR/WSAGetLastError/WSAE* symbols are Winsock-only -- they don't
// exist on those platforms, so building this unconditionally broke non-Windows CI.
#ifdef _WIN32
bool PeekPeerHungUp(SOCKET native_fd, bool& has_pending_data) {
    has_pending_data = false;

    int sock_type = 0;
    int sock_type_len = sizeof(sock_type);
    if (getsockopt(native_fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&sock_type),
                   &sock_type_len) == SOCKET_ERROR ||
        sock_type != SOCK_STREAM) {
        // Only a connected stream socket has a meaningful "hung up" concept; a listening or
        // UDP socket reading back "nothing ready" is unrelated and shouldn't be peeked.
        return false;
    }

    char peek_byte;
    const int peek_result = recv(native_fd, &peek_byte, 1, MSG_PEEK);
    if (peek_result > 0) {
        has_pending_data = true;
        return false;
    }
    if (peek_result == 0) {
        LOG_INFO(Network, "[Nextendo][DIAG] PeekPeerHungUp fd={} -- recv(MSG_PEEK) returned 0 "
                           "(orderly FIN), reporting hung up", native_fd);
        return true;
    }

    const int err = WSAGetLastError();
    switch (err) {
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENETRESET:
    case WSAESHUTDOWN:
        LOG_INFO(Network, "[Nextendo][DIAG] PeekPeerHungUp fd={} -- recv(MSG_PEEK) failed with "
                           "{}, reporting hung up", native_fd, err);
        return true;
    default:
        if (err != WSAEWOULDBLOCK) {
            LOG_INFO(Network, "[Nextendo][DIAG] PeekPeerHungUp fd={} -- recv(MSG_PEEK) failed "
                               "with unexpected {}, NOT reporting hung up", native_fd, err);
        }
        return false;
    }
}
#endif

} // Anonymous namespace

NetworkInstance::NetworkInstance() {
    Initialize();
}

NetworkInstance::~NetworkInstance() {
    Finalize();
}

void CancelPendingSocketOperations() {
    InterruptSocketOperations();
}

void RestartSocketOperations() {
    AcknowledgeInterrupt();
}

std::optional<IPv4Address> GetHostIPv4Address() {
    const auto network_interface = Network::GetSelectedNetworkInterface();
    if (!network_interface.has_value()) {
        // Only print the error once to avoid log spam
        static bool print_error = true;
        if (print_error) {
            LOG_ERROR(Network, "GetSelectedNetworkInterface returned no interface");
            print_error = false;
        }

        return {};
    }

    return TranslateIPv4(network_interface->ip_address);
}

std::string IPv4AddressToString(IPv4Address ip_addr) {
    std::array<char, INET_ADDRSTRLEN> buf = {};
    ASSERT(inet_ntop(AF_INET, &ip_addr, buf.data(), sizeof(buf)) == buf.data());
    return std::string(buf.data());
}

std::string IPv4AddressToRedactedString(IPv4Address ip_addr) {
    return fmt::format("{}.{}.x.x", ip_addr[0], ip_addr[1]);
}

// [Nextendo] A game can receive a peer's address as a literal IP string (e.g. baked into a
// matchmaking ticket) and hand it straight to getaddrinfo -- there is nothing to actually
// resolve. Falling through to a real lookup on an already-numeric host is exactly the bug
// Ryujinx-Nextendo's own DnsMitmResolver fix (2026-08-13) diagnosed for Splatoon 3's gRPC/NPLN
// connections: of the endpoints the game opens, only the one resolved by its real hostname
// worked -- the others completed TCP+TLS+HTTP/2 but never sent a HEADERS frame and closed
// after ~1s, because whatever canonical name a fallback lookup produced for an already-literal
// IP was some unrelated host (every redirected hostname shares the same IP, so any lookup that
// isn't a no-op returns whichever name happens to reverse-resolve for it), and the game builds
// its HTTP/2 :authority header from that corrupted name. Matches Ryujinx-Nextendo's own fix:
// return the literal address as-is, with itself as both address and hostname, and do it BEFORE
// any redirect/blocklist logic -- a literal IP was never a hostname to redirect or block.
bool TryParseIPv4Literal(const std::string& host, IPv4Address& out) {
    return inet_pton(AF_INET, host.c_str(), out.data()) == 1;
}

u32 IPv4AddressToInteger(IPv4Address ip_addr) {
    return static_cast<u32>(ip_addr[0]) << 24 | static_cast<u32>(ip_addr[1]) << 16 |
           static_cast<u32>(ip_addr[2]) << 8 | static_cast<u32>(ip_addr[3]);
}

Common::Expected<std::vector<AddrInfo>, GetAddrInfoError> GetAddressInfo(
    const std::string& host, const std::optional<std::string>& service) {
    addrinfo hints{};
    hints.ai_family = AF_INET; // Switch only supports IPv4.
    addrinfo* addrinfo;
    s32 gai_err = getaddrinfo(host.c_str(), service.has_value() ? service->c_str() : nullptr,
                              &hints, &addrinfo);
    if (gai_err != 0) {
        return Common::Unexpected(TranslateGetAddrInfoErrorFromNative(gai_err));
    }
    std::vector<AddrInfo> ret;
    for (auto* current = addrinfo; current; current = current->ai_next) {
        // We should only get AF_INET results due to the hints value.
        ASSERT_OR_EXECUTE(addrinfo->ai_family == AF_INET &&
                              addrinfo->ai_addrlen == sizeof(sockaddr_in),
                          continue;);

        AddrInfo& out = ret.emplace_back();
        out.family = TranslateDomainFromNative(current->ai_family);
        out.socket_type = TranslateTypeFromNative(current->ai_socktype);
        out.protocol = TranslateProtocolFromNative(current->ai_protocol);
        out.addr = TranslateToSockAddrIn(*reinterpret_cast<sockaddr_in*>(current->ai_addr),
                                         current->ai_addrlen);
        if (current->ai_canonname != nullptr) {
            out.canon_name = current->ai_canonname;
        }
    }
    freeaddrinfo(addrinfo);
    return ret;
}

std::pair<s32, Errno> Poll(std::vector<PollFD>& pollfds, s32 timeout) {
    const size_t num = pollfds.size();

    std::vector<WSAPOLLFD> host_pollfds(pollfds.size());
    std::transform(pollfds.begin(), pollfds.end(), host_pollfds.begin(), [](PollFD fd) {
        WSAPOLLFD result;
        result.fd = fd.socket->GetFD();
        result.events = TranslatePollEvents(fd.events);
        result.revents = 0;
        return result;
    });

    host_pollfds.push_back(WSAPOLLFD{
        .fd = GetInterruptSocket(),
        .events = POLLIN,
        .revents = 0,
    });

    const int result =
        WSAPoll(host_pollfds.data(), static_cast<ULONG>(host_pollfds.size()), timeout);
    if (result == SOCKET_ERROR) {
        return {-1, GetAndLogLastError()};
    }

    // [Nextendo] See PeekPeerHungUp's comment (WIN32-only -- WSAPoll's exceptfds gap doesn't
    // exist on POSIX poll()). Re-check every fd the caller asked to read from that WSAPoll
    // didn't already report ready -- regardless of whether WSAPoll returned 0 or reported some
    // other fd ready -- so a peer that hung up gets surfaced as ready-with-Hup instead of
    // silently polling as not-ready forever.
    s32 real_count = 0;
    for (size_t i = 0; i < num; ++i) {
        pollfds[i].revents = TranslatePollRevents(host_pollfds[i].revents);
#ifdef _WIN32
        if (True(pollfds[i].events & PollEvents::In) &&
            False(pollfds[i].revents & (PollEvents::In | PollEvents::Err | PollEvents::Hup))) {
            bool has_pending_data = false;
            if (PeekPeerHungUp(host_pollfds[i].fd, has_pending_data)) {
                pollfds[i].revents |= PollEvents::In | PollEvents::Hup;
            } else if (has_pending_data) {
                pollfds[i].revents |= PollEvents::In;
            }
        }
#endif
        if (pollfds[i].revents != PollEvents{}) {
            ++real_count;
        }
    }
    return {real_count, Errno::SUCCESS};
}

Socket::~Socket() {
    if (fd == INVALID_SOCKET) {
        return;
    }
    (void)closesocket(fd);
    fd = INVALID_SOCKET;
}

Socket::Socket(Socket&& rhs) noexcept {
    fd = std::exchange(rhs.fd, INVALID_SOCKET);
}

template <typename T>
std::pair<T, Errno> Socket::GetSockOpt(SOCKET fd_so, int option) {
    T value{};
    socklen_t len = sizeof(value);
    const int result = getsockopt(fd_so, SOL_SOCKET, option, reinterpret_cast<char*>(&value), &len);
    if (result != SOCKET_ERROR) {
        ASSERT(len == sizeof(value));
        return {value, Errno::SUCCESS};
    }
    return {value, GetAndLogLastError()};
}

template <typename T>
Errno Socket::SetSockOpt(SOCKET fd_so, int option, T value) {
    const int result =
        setsockopt(fd_so, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
    if (result != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }
    return GetAndLogLastError();
}

namespace {
void EnableAggressiveTcpKeepAlive(SOCKET fd) {
    constexpr u32 enable = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&enable),
               sizeof(enable));

#ifdef _WIN32
    tcp_keepalive vals{1, 5000, 5000};
    DWORD bytes_returned = 0;
    WSAIoctl(fd, SIO_KEEPALIVE_VALS, &vals, sizeof(vals), nullptr, 0, &bytes_returned, nullptr,
             nullptr);
#elif defined(__unix__) || defined(__APPLE__)
    constexpr u32 idle = 5;
    constexpr u32 interval = 5;
    constexpr u32 count = 3;
#ifdef __APPLE__
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#else
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}
} // Anonymous namespace

Errno Socket::Initialize(Domain domain, Type type, Protocol protocol) {
    fd = socket(TranslateDomainToNative(domain), TranslateTypeToNative(type),
                TranslateProtocolToNative(protocol));
    if (fd == INVALID_SOCKET) {
        return GetAndLogLastError();
    }

    // Stay ahead of NAT/firewall idle-connection drops regardless of whether the game asked.
    if (type == Type::STREAM) {
        EnableAggressiveTcpKeepAlive(fd);
    }

    return Errno::SUCCESS;
}

std::pair<SocketBase::AcceptResult, Errno> Socket::Accept() {
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    const bool wait_for_accept = !is_non_blocking;
    if (wait_for_accept) {
        std::vector<WSAPOLLFD> host_pollfds{
            WSAPOLLFD{fd, POLLIN, 0},
            WSAPOLLFD{GetInterruptSocket(), POLLIN, 0},
        };

        while (true) {
            const int pollres =
                WSAPoll(host_pollfds.data(), static_cast<ULONG>(host_pollfds.size()), -1);
            if (host_pollfds[1].revents != 0) {
                // Interrupt signaled before a client could be accepted, break
                return {AcceptResult{}, Errno::AGAIN};
            }
            if (pollres > 0) {
                break;
            }
        }
    }

    const SOCKET new_socket = accept(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen);

    if (new_socket == INVALID_SOCKET) {
        return {AcceptResult{}, GetAndLogLastError()};
    }

    AcceptResult result{
        .socket = std::make_unique<Socket>(new_socket),
        .sockaddr_in = TranslateToSockAddrIn(addr, addrlen),
    };

    return {std::move(result), Errno::SUCCESS};
}

Errno Socket::Connect(SockAddrIn addr_in) {
    const sockaddr host_addr_in = TranslateFromSockAddrIn(addr_in);
    if (connect(fd, &host_addr_in, sizeof(host_addr_in)) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    const Errno result = GetAndLogLastError(CallType::Connect);
    if (result != Errno::INPROGRESS) {
        return result;
    }

    // [Nextendo] Splatoon 3's gRPC connection stack doesn't survive Citron's honest
    // non-blocking connect()+EINPROGRESS+poll() cycle reliably -- something in its own
    // retry bookkeeping gives up long before the connection actually finishes, even
    // though the TCP handshake itself always succeeds. Ryujinx-Nextendo sidesteps this
    // entire class of timing issue by never handing the guest an "in progress" status
    // for this connect() at all: it blocks host-side until the handshake genuinely
    // completes and reports the definitive outcome directly (see Ryujinx's own log
    // line "grpc connect completed synchronously (blocked for host completion)").
    // Mirror that here with a bounded host-side wait instead of returning INPROGRESS.
    WSAPOLLFD wait_fd{};
    wait_fd.fd = fd;
    wait_fd.events = POLLOUT;
    wait_fd.revents = 0;
    constexpr int connect_wait_timeout_ms = 5000;
    const int poll_result = WSAPoll(&wait_fd, 1, connect_wait_timeout_ms);
    if (poll_result <= 0) {
        // Timed out (or the wait itself errored) -- fall back to the original
        // INPROGRESS status so the guest's own poll loop can still take over.
        return result;
    }

    const auto [pending_err, getsockopt_err] = GetPendingError();
    if (getsockopt_err != Errno::SUCCESS) {
        return result;
    }
    if (pending_err != Errno::SUCCESS) {
        LOG_ERROR(Network,
                  "[Nextendo] Connect blocked-wait finished with error, errno={}",
                  static_cast<int>(pending_err));
        return pending_err;
    }

    LOG_INFO(Network, "[Nextendo] grpc connect completed synchronously (blocked for host "
                      "completion) -> SUCCESS");
    return Errno::SUCCESS;
}

std::pair<SockAddrIn, Errno> Socket::GetPeerName() {
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == SOCKET_ERROR) {
        return {SockAddrIn{}, GetAndLogLastError()};
    }

    return {TranslateToSockAddrIn(addr, addrlen), Errno::SUCCESS};
}

std::pair<SockAddrIn, Errno> Socket::GetSockName() {
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == SOCKET_ERROR) {
        return {SockAddrIn{}, GetAndLogLastError()};
    }

    return {TranslateToSockAddrIn(addr, addrlen), Errno::SUCCESS};
}

Errno Socket::Bind(SockAddrIn addr) {
    const sockaddr addr_in = TranslateFromSockAddrIn(addr);
    if (bind(fd, &addr_in, sizeof(addr_in)) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

Errno Socket::Listen(s32 backlog) {
    if (listen(fd, backlog) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

Errno Socket::Shutdown(ShutdownHow how) {
    int host_how = 0;
    switch (how) {
    case ShutdownHow::RD:
        host_how = SD_RECEIVE;
        break;
    case ShutdownHow::WR:
        host_how = SD_SEND;
        break;
    case ShutdownHow::RDWR:
        host_how = SD_BOTH;
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented flag how={}", how);
        return Errno::SUCCESS;
    }
    if (shutdown(fd, host_how) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

std::pair<s32, Errno> Socket::Recv(int flags, std::span<u8> message) {
    ASSERT(flags == 0);
    ASSERT(message.size() < static_cast<size_t>(std::numeric_limits<int>::max()));

    const auto result =
        recv(fd, reinterpret_cast<char*>(message.data()), static_cast<int>(message.size()), 0);
    if (result != SOCKET_ERROR) {
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    return {-1, GetAndLogLastError()};
}

std::pair<s32, Errno> Socket::RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) {
    ASSERT(flags == 0);
    ASSERT(message.size() < static_cast<size_t>(std::numeric_limits<int>::max()));

    sockaddr_in addr_in{};
    socklen_t addrlen = sizeof(addr_in);
    socklen_t* const p_addrlen = addr ? &addrlen : nullptr;
    sockaddr* const p_addr_in = addr ? reinterpret_cast<sockaddr*>(&addr_in) : nullptr;

    const auto result = recvfrom(fd, reinterpret_cast<char*>(message.data()),
                                 static_cast<int>(message.size()), 0, p_addr_in, p_addrlen);
    if (result != SOCKET_ERROR) {
        if (addr) {
            *addr = TranslateToSockAddrIn(addr_in, addrlen);
        }
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    return {-1, GetAndLogLastError()};
}

std::pair<s32, Errno> Socket::Send(std::span<const u8> message, int flags) {
    ASSERT(message.size() < static_cast<size_t>(std::numeric_limits<int>::max()));
    ASSERT(flags == 0);

    int native_flags = 0;
#if defined(__unix__) || defined(__APPLE__)
    native_flags |= MSG_NOSIGNAL; // do not send us SIGPIPE
#endif
    const auto result = send(fd, reinterpret_cast<const char*>(message.data()),
                             static_cast<int>(message.size()), native_flags);
    if (result != SOCKET_ERROR) {
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    return {-1, GetAndLogLastError(CallType::Send)};
}

std::pair<s32, Errno> Socket::SendTo(u32 flags, std::span<const u8> message,
                                     const SockAddrIn* addr) {
    ASSERT(flags == 0);

    const sockaddr* to = nullptr;
    const int to_len = addr ? sizeof(sockaddr) : 0;
    sockaddr host_addr_in;

    if (addr) {
        host_addr_in = TranslateFromSockAddrIn(*addr);
        to = &host_addr_in;
    }

    const auto result = sendto(fd, reinterpret_cast<const char*>(message.data()),
                               static_cast<int>(message.size()), 0, to, to_len);
    if (result != SOCKET_ERROR) {
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    return {-1, GetAndLogLastError(CallType::Send)};
}

Errno Socket::Close() {
    [[maybe_unused]] const int result = closesocket(fd);
    ASSERT(result == 0);
    fd = INVALID_SOCKET;

    return Errno::SUCCESS;
}

std::pair<Errno, Errno> Socket::GetPendingError() {
    auto [pending_err, getsockopt_err] = GetSockOpt<int>(fd, SO_ERROR);
    return {TranslateNativeError(pending_err), getsockopt_err};
}

Errno Socket::SetLinger(bool enable, u32 linger) {
    return SetSockOpt(fd, SO_LINGER, MakeLinger(enable, linger));
}

std::tuple<bool, u32, Errno> Socket::GetLinger() {
    auto [value, err] = GetSockOpt<decltype(MakeLinger(false, 0))>(fd, SO_LINGER);
    if (err != Errno::SUCCESS) {
        return {false, 0, err};
    }
    return {value.l_onoff != 0, static_cast<u32>(value.l_linger), Errno::SUCCESS};
}

Errno Socket::SetReuseAddr(bool enable) {
    return SetSockOpt<u32>(fd, SO_REUSEADDR, enable ? 1 : 0);
}

Errno Socket::SetKeepAlive(bool enable) {
    return SetSockOpt<u32>(fd, SO_KEEPALIVE, enable ? 1 : 0);
}

Errno Socket::SetBroadcast(bool enable) {
    return SetSockOpt<u32>(fd, SO_BROADCAST, enable ? 1 : 0);
}

// [Nextendo] See the declaration comment in sockets.h.
std::pair<bool, Errno> Socket::GetReuseAddr() {
    auto [value, err] = GetSockOpt<u32>(fd, SO_REUSEADDR);
    return {value != 0, err};
}

std::pair<bool, Errno> Socket::GetKeepAlive() {
    auto [value, err] = GetSockOpt<u32>(fd, SO_KEEPALIVE);
    return {value != 0, err};
}

std::pair<bool, Errno> Socket::GetBroadcast() {
    auto [value, err] = GetSockOpt<u32>(fd, SO_BROADCAST);
    return {value != 0, err};
}

Errno Socket::SetSndBuf(u32 value) {
    return SetSockOpt(fd, SO_SNDBUF, value);
}

Errno Socket::SetRcvBuf(u32 value) {
    return SetSockOpt(fd, SO_RCVBUF, value);
}

std::pair<u32, Errno> Socket::GetSndBuf() {
    return GetSockOpt<u32>(fd, SO_SNDBUF);
}

std::pair<u32, Errno> Socket::GetRcvBuf() {
    return GetSockOpt<u32>(fd, SO_RCVBUF);
}

// [Nextendo] SO_SNDTIMEO/SO_RCVTIMEO take a raw DWORD (milliseconds) on Winsock, but POSIX
// requires a struct timeval{tv_sec, tv_usec} instead -- passing the generic SetSockOpt<u32>'s
// 4-byte int there fails setsockopt with EINVAL (confirmed live: a parked UDP socket's drain
// thread call site never checked this Errno, so on Linux the timeout silently never applied and
// its background RecvFrom blocked forever, hanging StopDrain()'s join() the first time a parked
// socket needed to be reclaimed or expired -- P2P titles wedge shortly after their first NAT
// hole-punch).
#ifdef _WIN32
// [Nextendo] SO_SNDTIMEO/SO_RCVTIMEO take a raw DWORD on Winsock but a struct timeval on POSIX;
// the generic SetSockOpt<u32> only matches the Windows side.
#ifdef _WIN32
Errno Socket::SetSndTimeo(u32 value) {
    return SetSockOpt(fd, SO_SNDTIMEO, value);
}

Errno Socket::SetRcvTimeo(u32 value) {
    return SetSockOpt(fd, SO_RCVTIMEO, value);
}
#elif defined(__unix__) || defined(__APPLE__)
namespace {
Errno SetTimevalSockOpt(SOCKET fd_so, int option, u32 milliseconds) {
    struct timeval tv {};
    tv.tv_sec = static_cast<time_t>(milliseconds / 1000);
    tv.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
    const int result = setsockopt(fd_so, SOL_SOCKET, option, &tv, sizeof(tv));
    if (result != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }
    return GetAndLogLastError();
}
} // namespace

Errno Socket::SetSndTimeo(u32 value) {
    return SetTimevalSockOpt(fd, SO_SNDTIMEO, value);
}

Errno Socket::SetRcvTimeo(u32 value) {
    return SetTimevalSockOpt(fd, SO_RCVTIMEO, value);
}
#endif
#elif defined(__unix__) || defined(__APPLE__)
namespace {
Errno SetTimevalSockOpt(SOCKET fd_so, int option, u32 milliseconds) {
    struct timeval tv {};
    tv.tv_sec = static_cast<time_t>(milliseconds / 1000);
    tv.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
    const int result = setsockopt(fd_so, SOL_SOCKET, option, &tv, sizeof(tv));
    if (result != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }
    return GetAndLogLastError();
}
} // namespace

Errno Socket::SetSndTimeo(u32 value) {
    return SetTimevalSockOpt(fd, SO_SNDTIMEO, value);
}

Errno Socket::SetRcvTimeo(u32 value) {
    return SetTimevalSockOpt(fd, SO_RCVTIMEO, value);
}
#endif

Errno Socket::SetNonBlock(bool enable) {
    if (EnableNonBlock(fd, enable)) {
        is_non_blocking = enable;
        return Errno::SUCCESS;
    }
    return GetAndLogLastError();
}

// [Nextendo] IPPROTO_TCP-level, so these can't go through the generic SetSockOpt/GetSockOpt
// helpers above (they hardcode SOL_SOCKET). See the declaration comment in sockets.h.
Errno Socket::SetNoDelay(bool enable) {
    const int value = enable ? 1 : 0;
    const int result = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value),
                                   sizeof(value));
    if (result != SOCKET_ERROR) {
        no_delay_cache = enable;
        return Errno::SUCCESS;
    }
    return GetAndLogLastError();
}

// [Nextendo] getsockopt(IPPROTO_TCP, TCP_NODELAY) on a socket that hasn't finished connect()
// yet can come back with a short/zero-length value on Windows (confirmed live: the length
// check below fails on every connect cycle for this exact call). Since `value` is
// zero-initialized, a short write there silently reports nodelay=false even right after the
// guest set it true. Prefer the last value the guest itself set -- SetNoDelay already proved
// it took by checking setsockopt's own return -- over a getsockopt call whose length we can't
// trust; only fall back to the raw read if the guest never called SetNoDelay on this socket.
std::pair<bool, Errno> Socket::GetNoDelay() {
    int value = 0;
    socklen_t len = sizeof(value);
    const int result =
        getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&value), &len);
    if (result != SOCKET_ERROR) {
        if (len != sizeof(value)) {
            if (no_delay_cache) {
                return {*no_delay_cache, Errno::SUCCESS};
            }
            LOG_WARNING(Network, "getsockopt(TCP_NODELAY) returned len={} (expected {}), and no "
                                  "prior SetNoDelay to fall back on -- reporting false",
                        len, sizeof(value));
            return {false, Errno::SUCCESS};
        }
        return {value != 0, Errno::SUCCESS};
    }
    return {false, GetAndLogLastError()};
}

bool Socket::IsOpened() const {
    return fd != INVALID_SOCKET;
}

void Socket::HandleProxyPacket(const ProxyPacket& packet) {
    LOG_WARNING(Network,
                "ProxyPacket received on regular socket (not ProxySocket). "
                "This may indicate socket type mismatch. "
                "Packet from {}:{} to {}:{}, protocol={}",
                packet.local_endpoint.ip[0], packet.local_endpoint.portno,
                packet.remote_endpoint.ip[0], packet.remote_endpoint.portno,
                static_cast<int>(packet.protocol));
}

} // namespace Network
