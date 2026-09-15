// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/hex_util.h"
#include "common/nextendo_nat.h"
#include "common/settings.h"
#include "common/socket_types.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_process_page_table.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc/nextendo_deadline_watch.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/hle/service/ssl/ssl_pending_registry.h"
#include "core/internal_network/network.h"
#include "core/internal_network/socket_proxy.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"

using Common::Expected;
using Common::Unexpected;

namespace Service::Sockets {

namespace {

// A parked socket's peer can keep sending real P2P data (island/session traffic, not just NAT
// probes) while nothing is reading it. The original parking scheme only survived being
// reclaimed once: a second close-without-rebind left the socket parked forever with no thread
// ever calling RecvFrom on it again, so the guest simply stopped seeing data that was arriving
// fine at the OS level (confirmed via a live ACNH repro: real data flowed for ~1.5s, the game
// closed the socket a second time, and every packet afterward vanished into the void). This
// drain thread keeps consuming datagrams the whole time a socket is parked, so a second close
// degrades to "temporarily buffered" instead of "silently dropped forever".
struct ParkedUdpDrain {
    std::mutex mutex;
    std::deque<std::pair<std::vector<u8>, Network::SockAddrIn>> queued;
    std::atomic<bool> stop{false};
    std::thread thread;
};

constexpr std::size_t MAX_PARKED_UDP_QUEUE = 256;
// SO_RCVTIMEO takes milliseconds (Socket::SetRcvTimeo forwards this value straight into
// setsockopt), NOT microseconds — 200 here, not 200'000, or StopDrain()'s join() can block
// the caller (a guest service-thread Bind()/Close() call) for the better part of 200 SECONDS
// waiting on a blocking RecvFrom that has nothing to receive.
constexpr u32 PARKED_DRAIN_POLL_TIMEOUT_MS = 200;

static void StopDrain(ParkedUdpDrain& drain) {
    drain.stop.store(true, std::memory_order_relaxed);
    if (drain.thread.joinable()) {
        drain.thread.join();
    }
}

static void StartDrain(ParkedUdpDrain& drain, std::shared_ptr<Network::SocketBase> socket) {
    socket->SetRcvTimeo(PARKED_DRAIN_POLL_TIMEOUT_MS);
    drain.thread = std::thread([&drain, socket = std::move(socket)] {
        while (!drain.stop.load(std::memory_order_relaxed)) {
            std::vector<u8> buffer(2048);
            Network::SockAddrIn addr{};
            const auto [ret, err] = socket->RecvFrom(0, buffer, &addr);
            if (ret <= 0) {
                // Timeout (nothing arrived this poll) or the socket genuinely died — either
                // way, loop back and check `stop` again rather than spinning on an error.
                continue;
            }
            buffer.resize(static_cast<size_t>(ret));

            std::lock_guard lock(drain.mutex);
            if (drain.queued.size() >= MAX_PARKED_UDP_QUEUE) {
                drain.queued.pop_front();
            }
            drain.queued.emplace_back(std::move(buffer), addr);
        }
    });
}

struct ParkedUdpSocket {
    std::shared_ptr<Network::SocketBase> socket;
    u16 port;
    std::chrono::steady_clock::time_point park_time;
    std::unique_ptr<ParkedUdpDrain> drain;
};

static std::mutex g_parked_udp_mutex;
static std::vector<ParkedUdpSocket> g_parked_udp_sockets;

constexpr std::size_t MAX_PARKED_UDP_SOCKETS = 16;
constexpr auto PARK_DURATION = std::chrono::seconds{60};

// Caller holds g_parked_udp_mutex.
static void DropExpiredParkedUdpSockets(std::chrono::steady_clock::time_point now) {
    std::erase_if(g_parked_udp_sockets, [now](ParkedUdpSocket& p) {
        if (now - p.park_time <= PARK_DURATION) {
            return false;
        }
        StopDrain(*p.drain);
        p.socket->Close();
        return true;
    });
}

static bool ParkUdpSocket(std::shared_ptr<Network::SocketBase> socket, u16 port) {
    if (!socket || port == 0) {
        return false;
    }

    std::lock_guard lock(g_parked_udp_mutex);
    const auto now = std::chrono::steady_clock::now();
    DropExpiredParkedUdpSockets(now);

    // Keep the newer socket: it carries the mapping the guest was last using.
    const auto existing = std::ranges::find_if(
        g_parked_udp_sockets, [port](const ParkedUdpSocket& p) { return p.port == port; });
    if (existing != g_parked_udp_sockets.end()) {
        StopDrain(*existing->drain);
        existing->socket->Close();
        g_parked_udp_sockets.erase(existing);
    } else if (g_parked_udp_sockets.size() >= MAX_PARKED_UDP_SOCKETS) {
        return false;
    }

    auto drain = std::make_unique<ParkedUdpDrain>();
    StartDrain(*drain, socket);
    g_parked_udp_sockets.push_back({std::move(socket), port, now, std::move(drain)});
    return true;
}

// Returns the reclaimed socket plus whatever datagrams its drain thread buffered while parked,
// oldest first, so the caller can replay them ahead of any new live read.
static std::pair<std::shared_ptr<Network::SocketBase>,
                  std::deque<std::pair<std::vector<u8>, Network::SockAddrIn>>>
TakeParkedUdpSocket(u16 port) {
    if (port == 0) {
        return {};
    }

    std::lock_guard lock(g_parked_udp_mutex);
    const auto now = std::chrono::steady_clock::now();
    DropExpiredParkedUdpSockets(now);

    const auto it = std::ranges::find_if(
        g_parked_udp_sockets, [port](const ParkedUdpSocket& p) { return p.port == port; });
    if (it == g_parked_udp_sockets.end()) {
        return {};
    }

    StopDrain(*it->drain);
    auto socket = it->socket;
    auto queued = std::move(it->drain->queued);
    g_parked_udp_sockets.erase(it);
    return {std::move(socket), std::move(queued)};
}

void ClearParkedUdpSockets() {
    std::lock_guard lock(g_parked_udp_mutex);
    for (auto& parked : g_parked_udp_sockets) {
        StopDrain(*parked.drain);
        parked.socket->Close();
    }
    g_parked_udp_sockets.clear();
}

// [Nextendo] A connected TCP socket's peer can still have a legitimate, already-in-flight
// reply on the wire at the exact moment the guest calls Close() -- confirmed live via packet
// capture on Splatoon 3's NPLN/gRPC traffic: the guest sends its request, immediately calls
// shutdown(SD_BOTH) then Close() (a few ms apart), and ~90-100ms later the real server's
// response finally arrives... to a connection the OS no longer has any record of, because
// closesocket() had already run. Per RFC 793, any segment for a connection missing from the
// OS's table gets an unconditional RST back -- confirmed in the capture (client sends [RST]
// the instant the late FIN+data arrives). The guest already told the OS it's done with both
// directions (shutdown(SD_BOTH)), so it will never read this reply either way -- SO_LINGER
// doesn't help here (it only governs flushing the LOCAL machine's own outgoing bytes, and
// those had already been fully handed to the OS before Close() was even called; it has
// nothing to do with waiting for a peer's incoming reply). The actual fix is simpler: don't
// let the real closesocket() run the instant the guest asks -- keep the OS-level socket
// resource alive a short grace period first, so the kernel can still gracefully ACK a
// last-second reply (and any FIN that comes with it) instead of RSTing it. The guest-visible
// fd is already freed synchronously in CloseImpl before this runs, so this changes nothing
// the guest can observe -- it only delays when the real host resource is released.
constexpr auto TCP_CLOSE_GRACE = std::chrono::milliseconds{500};

void DeferredCloseTcpSocket(std::shared_ptr<Network::SocketBase> socket) {
    std::thread([socket = std::move(socket)]() mutable {
        std::this_thread::sleep_for(TCP_CLOSE_GRACE);
        socket->Close();
    }).detach();
}

// Best-effort PRUDP-Lite header decode for P2P match traffic (SYN/CONNECT/DATA/DISCONNECT/PING +
// flags). Only the header is plaintext; the RMC payload inside stays opaque. Returns empty for
// anything that isn't a PRUDP-Lite packet (magic 0x80, >=12 bytes) so callers can skip it.
std::string DescribePrudpLite(std::span<const u8> data) {
    static constexpr std::array<const char*, 5> type_names{"SYN", "CONNECT", "DATA", "DISCONNECT",
                                                            "PING"};
    if (data.size() < 12 || data[0] != 0x80) {
        // TEMPORARY DIAGNOSTIC: dump the full packet (not just a 16-byte prefix) so a non-PRUDP-Lite
        // protocol (e.g. Pia P2P) can actually be read back out of the log instead of guessed at.
        const auto head = data.subspan(0, std::min<size_t>(data.size(), 1500));
        return fmt::format("raw[{}]={}", data.size(), Common::HexToString(head, false));
    }
    const u16 type_flags = static_cast<u16>(data[8] | (data[9] << 8));
    const u8 type = type_flags & 0xF;
    const u16 flags = type_flags >> 4;
    std::string out = fmt::format("prudp type={}", type < type_names.size() ? type_names[type]
                                                                            : std::to_string(type));
    if (flags & 0x001) out += "|ACK";
    if (flags & 0x002) out += "|Reliable";
    if (flags & 0x004) out += "|NeedACK";
    if (flags & 0x200) out += "|MultiACK";
    out += fmt::format(" id={}", static_cast<u16>(data[10] | (data[11] << 8)));
    return out;
}

static bool TryInjectTlsSni(std::span<const u8> input, const std::string& host_name, std::vector<u8>& output) {
    if (input.size() < 43 || input[0] != 0x16) return false;
    size_t recordLen = (static_cast<size_t>(input[3]) << 8) | input[4];
    if (5 + recordLen > input.size()) return false;
    if (input[5] != 0x01) return false; // ClientHello

    size_t p = 5 + 4 + 2 + 32; // record header (5) + handshake header (4) + version (2) + random (32)
    if (p >= input.size()) return false;
    size_t sidLen = input[p]; p += 1 + sidLen;
    if (p + 2 > input.size()) return false;
    size_t csLen = (static_cast<size_t>(input[p]) << 8) | input[p + 1]; p += 2 + csLen;
    if (p + 1 > input.size()) return false;
    size_t cmLen = input[p]; p += 1 + cmLen;
    if (p + 2 > input.size()) return false;

    size_t extTotalLen = (static_cast<size_t>(input[p]) << 8) | input[p + 1];
    size_t extLenPos = p;
    size_t extStart = p + 2;
    size_t extEnd = extStart + extTotalLen;
    if (extEnd > input.size()) return false;

    // Check if server_name (0x0000) extension already exists
    size_t q = extStart;
    while (q + 4 <= extEnd) {
        u16 etype = static_cast<u16>((input[q] << 8) | input[q + 1]);
        u16 elen = static_cast<u16>((input[q + 2] << 8) | input[q + 3]);
        if (etype == 0x0000) return false; // already has SNI
        q += 4 + elen;
    }

    // Build SNI extension bytes
    const size_t nameLen = host_name.size();
    const size_t listLen = 1 + 2 + nameLen;
    const size_t extDataLen = 2 + listLen;
    const size_t sniExtLen = 4 + extDataLen;

    std::vector<u8> sni(sniExtLen);
    size_t i = 0;
    sni[i++] = 0x00; sni[i++] = 0x00;
    sni[i++] = static_cast<u8>(extDataLen >> 8); sni[i++] = static_cast<u8>(extDataLen);
    sni[i++] = static_cast<u8>(listLen >> 8); sni[i++] = static_cast<u8>(listLen);
    sni[i++] = 0x00;
    sni[i++] = static_cast<u8>(nameLen >> 8); sni[i++] = static_cast<u8>(nameLen);
    std::memcpy(sni.data() + i, host_name.data(), nameLen);

    output.resize(input.size() + sniExtLen);
    std::memcpy(output.data(), input.data(), extStart);
    std::memcpy(output.data() + extStart, sni.data(), sniExtLen);
    std::memcpy(output.data() + extStart + sniExtLen, input.data() + extStart, input.size() - extStart);

    size_t newExtLen = extTotalLen + sniExtLen;
    output[extLenPos] = static_cast<u8>(newExtLen >> 8);
    output[extLenPos + 1] = static_cast<u8>(newExtLen);

    size_t hsLen = ((input[6] << 16) | (input[7] << 8) | input[8]) + sniExtLen;
    output[6] = static_cast<u8>(hsLen >> 16); output[7] = static_cast<u8>(hsLen >> 8); output[8] = static_cast<u8>(hsLen);

    size_t newRecLen = recordLen + sniExtLen;
    output[3] = static_cast<u8>(newRecLen >> 8); output[4] = static_cast<u8>(newRecLen);

    return true;
}

// Queued from an earlier send's ICMP error, not from this receive.
bool IsTransientDatagramError(Errno bsd_errno) {
    return bsd_errno == Errno::CONNREFUSED || bsd_errno == Errno::CONNRESET;
}

bool IsConnectionBased(Type type) {
    switch (type) {
    case Type::STREAM:
        return true;
    case Type::DGRAM:
        return false;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return false;
    }
}

template <typename T>
T GetValue(std::span<const u8> buffer) {
    T t{};
    std::memcpy(&t, buffer.data(), std::min(sizeof(T), buffer.size()));
    return t;
}

template <typename T>
void PutValue(std::span<u8> buffer, const T& t) {
    std::memcpy(buffer.data(), &t, std::min(sizeof(T), buffer.size()));
}

class OfflineSocket final : public Network::SocketBase {
public:
    Network::Errno Initialize(Network::Domain domain_, Network::Type type_,
                              Network::Protocol protocol_) override {
        domain = domain_;
        type = type_;
        protocol = protocol_;
        return Network::Errno::SUCCESS;
    }

    Network::Errno Close() override {
        opened = false;
        return Network::Errno::SUCCESS;
    }

    std::pair<AcceptResult, Network::Errno> Accept() override {
        return {AcceptResult{}, Network::Errno::NETDOWN};
    }

    Network::Errno Connect(Network::SockAddrIn) override {
        return Network::Errno::NETDOWN;
    }

    std::pair<Network::SockAddrIn, Network::Errno> GetPeerName() override {
        return {{}, Network::Errno::NOTCONN};
    }

    std::pair<Network::SockAddrIn, Network::Errno> GetSockName() override {
        return {{}, Network::Errno::SUCCESS};
    }

    Network::Errno Bind(Network::SockAddrIn) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno Listen(s32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno Shutdown(Network::ShutdownHow) override {
        return Network::Errno::SUCCESS;
    }

    std::pair<s32, Network::Errno> Recv(int, std::span<u8>) override {
        return {-1, Network::Errno::AGAIN};
    }

    std::pair<s32, Network::Errno> RecvFrom(int, std::span<u8>, Network::SockAddrIn*) override {
        return {-1, Network::Errno::AGAIN};
    }

    std::pair<s32, Network::Errno> Send(std::span<const u8> message, int) override {
        return {static_cast<s32>(message.size()), Network::Errno::SUCCESS};
    }

    std::pair<s32, Network::Errno> SendTo(u32, std::span<const u8> message,
                                          const Network::SockAddrIn*) override {
        return {static_cast<s32>(message.size()), Network::Errno::SUCCESS};
    }

    Network::Errno SetLinger(bool, u32) override {
        return Network::Errno::SUCCESS;
    }

    std::tuple<bool, u32, Network::Errno> GetLinger() override {
        return {false, 0, Network::Errno::SUCCESS};
    }

    Network::Errno SetReuseAddr(bool) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetKeepAlive(bool) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetBroadcast(bool) override {
        return Network::Errno::SUCCESS;
    }

    std::pair<bool, Network::Errno> GetReuseAddr() override {
        return {true, Network::Errno::SUCCESS};
    }

    std::pair<bool, Network::Errno> GetKeepAlive() override {
        return {true, Network::Errno::SUCCESS};
    }

    std::pair<bool, Network::Errno> GetBroadcast() override {
        return {true, Network::Errno::SUCCESS};
    }

    Network::Errno SetSndBuf(u32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetRcvBuf(u32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetSndTimeo(u32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetRcvTimeo(u32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetNonBlock(bool) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetNoDelay(bool enable) override {
        no_delay = enable;
        return Network::Errno::SUCCESS;
    }

    std::pair<bool, Network::Errno> GetNoDelay() override {
        return {no_delay, Network::Errno::SUCCESS};
    }

    std::pair<Network::Errno, Network::Errno> GetPendingError() override {
        return {Network::Errno::SUCCESS, Network::Errno::SUCCESS};
    }

    bool IsOpened() const override {
        return opened;
    }

    void HandleProxyPacket(const Network::ProxyPacket&) override {}

private:
    Network::Domain domain = Network::Domain::INET;
    Network::Type type = Network::Type::DGRAM;
    Network::Protocol protocol = Network::Protocol::UDP;
    bool opened = true;
    bool no_delay = false;
};

} // Anonymous namespace

void BSD::PollWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->PollImpl(write_buffer, read_buffer, nfds, timeout);
}

void BSD::PollWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::SelectWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) =
        bsd->SelectImpl(nfds, timeout, read_in, write_in, error_in, read_out, write_out, error_out);
}

void BSD::SelectWork::Response(HLERequestContext& ctx) {
    if (read_out.size() > 0) {
        ctx.WriteBuffer(read_out, 0);
    }
    if (write_out.size() > 0) {
        ctx.WriteBuffer(write_out, 1);
    }
    if (error_out.size() > 0) {
        ctx.WriteBuffer(error_out, 2);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::AcceptWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->AcceptImpl(fd, write_buffer);
}

void BSD::AcceptWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD::ConnectWork::Execute(BSD* bsd) {
    bsd_errno = bsd->ConnectImpl(fd, addr);
}

void BSD::ConnectWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD::RecvWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvImpl(fd, flags, message);
}

void BSD::RecvWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::RecvFromWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvFromImpl(fd, flags, message, addr);
}

void BSD::RecvFromWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message, 0);
    if (!addr.empty()) {
        ctx.WriteBuffer(addr, 1);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(addr.size()));
}

void BSD::SendWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendImpl(fd, flags, message);
}

void BSD::SendWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::SendToWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendToImpl(fd, flags, message, addr);
}

void BSD::SendToWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::RegisterClient(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    // Read LibraryConfigData structure
    struct LibraryConfigData {
        u32 version;
        u32 tcp_tx_buf_size;
        u32 tcp_rx_buf_size;
        u32 tcp_tx_buf_max_size;
        u32 tcp_rx_buf_max_size;
        u32 udp_tx_buf_size;
        u32 udp_rx_buf_size;
        u32 sb_efficiency;
    };

    const auto config = rp.PopRaw<LibraryConfigData>();
    const u64 transfer_memory_size = rp.Pop<u64>();
    [[maybe_unused]] const auto transfer_memory_handle = ctx.GetCopyHandle(0);
    const u64 pid = ctx.GetPID();

    LOG_INFO(Service, "called, version={} pid={} transfer_memory_size={:#x}",
             config.version, pid, transfer_memory_size);
    LOG_DEBUG(Service, "  TCP: tx={:#x} rx={:#x} tx_max={:#x} rx_max={:#x}",
              config.tcp_tx_buf_size, config.tcp_rx_buf_size,
              config.tcp_tx_buf_max_size, config.tcp_rx_buf_max_size);
    LOG_DEBUG(Service, "  UDP: tx={:#x} rx={:#x} sb_efficiency={}",
              config.udp_tx_buf_size, config.udp_rx_buf_size, config.sb_efficiency);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

void BSD::StartMonitoring(HLERequestContext& ctx) {
    LOG_INFO(Service, "called");

    // StartMonitoring initializes network event monitoring for BSD sockets
    // This command has no documented input parameters in switchbrew
    // It enables proper event handling for socket operations
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void BSD::Socket(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    const auto [fd, bsd_errno] = SocketImpl(static_cast<Domain>(domain), static_cast<Type>(type),
                                            static_cast<Protocol>(protocol));

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD::Select(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);

    ExecuteWork(ctx, SelectWork{
                         .nfds = nfds,
                         .timeout = timeout,
                         .read_in = ctx.CanReadBuffer(0) ? ctx.ReadBuffer(0) : std::span<const u8>{},
                         .write_in = ctx.CanReadBuffer(1) ? ctx.ReadBuffer(1) : std::span<const u8>{},
                         .error_in = ctx.CanReadBuffer(2) ? ctx.ReadBuffer(2) : std::span<const u8>{},
                         .read_out = std::vector<u8>(ctx.GetWriteBufferSize(0)),
                         .write_out = std::vector<u8>(ctx.GetWriteBufferSize(1)),
                         .error_out = std::vector<u8>(ctx.GetWriteBufferSize(2)),
                     });
}

void BSD::Poll(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);

    // [Nextendo] See sockets.h's deferred_poll_snapshots declaration comment: if this ctx was
    // already deferred once, reuse the pollfd bytes captured back then instead of re-reading
    // live guest memory now, which another IPC call could have reused in the meantime.
    std::vector<u8> read_buffer;
    bool had_snapshot = false;
    std::optional<std::chrono::steady_clock::time_point> existing_deadline;
    {
        std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
        if (const auto it = deferred_poll_snapshots.find(&ctx); it != deferred_poll_snapshots.end()) {
            read_buffer = it->second.read_buffer;
            existing_deadline = it->second.deadline;
            had_snapshot = true;
        }
    }
    if (!had_snapshot) {
        const auto live_buffer = ctx.ReadBuffer();
        read_buffer.assign(live_buffer.begin(), live_buffer.end());
    }

    // [Nextendo] See sockets.h's SetBsdDeferralEvent declaration comment. Covers any nonzero
    // timeout (matching Ryujinx-Nextendo's own `timeout != 0` condition), not just an infinite
    // one -- a bounded wait's own deadline is tracked in DeferredPollState and enforced below on
    // each re-check, so deferring it can't silently turn it into an unbounded wait. Also requires
    // an eventfd in the set and a deferral event to wait on; an ordinary title's poll (no eventfd,
    // e.g. Splatoon 2/MK8/SSBU) is untouched and still blocks exactly as before, matching
    // Ryujinx-Nextendo's own NEX-vs-gRPC split for this same failure.
    if (timeout != 0 && GetBsdDeferralEvent() != nullptr &&
        PollSetIncludesEventFd(read_buffer, nfds)) {
        std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
        auto [ret, bsd_errno] = PollImpl(write_buffer, read_buffer, nfds, /*timeout=*/0);

        // [Nextendo] Deadline expired while nothing became ready -- report ETIMEDOUT instead of
        // deferring again, exactly like a real bounded poll() would once its time is up.
        if (ret == 0 && bsd_errno == Errno::SUCCESS && existing_deadline &&
            std::chrono::steady_clock::now() >= *existing_deadline) {
            std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
            deferred_poll_snapshots.erase(&ctx);
            IPC::ResponseBuilder rb{ctx, 4};
            rb.Push(ResultSuccess);
            rb.Push<s32>(0);
            rb.PushEnum(Errno::SUCCESS);
            return;
        }

        if (ret == 0 && bsd_errno == Errno::SUCCESS) {
            // Nothing ready yet -- give up this thread instead of blocking it, so BSD's other
            // worker threads (and this one) stay free to service the eventfd Write() IPC that
            // would end this wait. ServerManager::CompleteSyncRequest re-invokes this handler
            // (re-parsing the same, still-buffered request) whenever the deferral event fires.
            if (!had_snapshot) {
                std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
                const auto deadline = timeout == -1
                                          ? std::optional<std::chrono::steady_clock::time_point>{}
                                          : std::optional{std::chrono::steady_clock::now() +
                                                           std::chrono::milliseconds(timeout)};
                deferred_poll_snapshots[&ctx] = DeferredPollState{read_buffer, deadline};
            }
            LOG_DEBUG(Service, "[Nextendo] Poll deferred (nfds={} timeout={}), eventfd in set",
                      nfds, timeout);
            ctx.SetIsDeferred();
            return;
        }
        // Completing now (successfully or with an error) -- drop the snapshot, if any.
        if (had_snapshot) {
            std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
            deferred_poll_snapshots.erase(&ctx);
        }
        // Already had something to report (or an error) -- same response shape as the normal
        // (non-deferred) path below, just without going through Network::Poll's own blocking
        // wait since PollImpl above already did the check.
        if (write_buffer.size() > 0) {
            ctx.WriteBuffer(write_buffer);
        }
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(ret);
        rb.PushEnum(bsd_errno);
        return;
    }

    // [Nextendo] Not taking the deferred path this call -- make sure no stale snapshot lingers
    // under this ctx pointer (defensive: guards against an HLERequestContext address ever being
    // reused for a genuinely different, later request).
    if (had_snapshot) {
        std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
        deferred_poll_snapshots.erase(&ctx);
    }

    ExecuteWork(ctx, PollWork{
                         .nfds = nfds,
                         .timeout = timeout,
                         .read_buffer = read_buffer,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD::Accept(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    ExecuteWork(ctx, AcceptWork{
                         .fd = fd,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD::Bind(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());
    BuildErrnoResponse(ctx, BindImpl(fd, ctx.ReadBuffer()));
}

void BSD::Connect(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, ConnectWork{
                         .fd = fd,
                         .addr = ctx.ReadBuffer(),
                     });
}

void BSD::GetPeerName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetPeerNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD::GetSockName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetSockNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD::GetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const auto optname = static_cast<OptName>(rp.Pop<u32>());

    std::vector<u8> optval(ctx.GetWriteBufferSize());

    LOG_DEBUG(Service, "called. fd={} level={} optname=0x{:x} len=0x{:x}", fd, level, optname,
              optval.size());

    const Errno err = GetSockOptImpl(fd, level, optname, optval);

    ctx.WriteBuffer(optval);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(err == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(err);
    rb.Push<u32>(static_cast<u32>(optval.size()));
}

void BSD::Listen(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 backlog = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} backlog={}", fd, backlog);

    BuildErrnoResponse(ctx, ListenImpl(fd, backlog));
}

void BSD::Fcntl(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 cmd = rp.Pop<u32>();
    const s32 arg = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} cmd={} arg={}", fd, cmd, arg);

    const auto [ret, bsd_errno] = FcntlImpl(fd, static_cast<FcntlCmd>(cmd), arg);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::SetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const OptName optname = static_cast<OptName>(rp.Pop<u32>());
    const auto optval = ctx.ReadBuffer();

    LOG_DEBUG(Service, "called. fd={} level={} optname=0x{:x} optlen={}", fd, level,
              static_cast<u32>(optname), optval.size());

    BuildErrnoResponse(ctx, SetSockOptImpl(fd, level, optname, optval));
}

void BSD::Shutdown(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const s32 how = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} how={}", fd, how);

    // [Nextendo][DIAG] Real guest call stack at the exact moment Shutdown() is called, for the
    // socket that had its first ClientHello sent (sni_injected) -- to see which guest code
    // actually makes this decision, directly, instead of inferring it from timing/absence of
    // other activity.
    if (IsFileDescriptorValid(fd) && file_descriptors[fd]->sni_injected) {
        const auto backtrace = Core::GetBacktrace(&ctx.GetThread());
        std::string trace_str;
        for (const auto& entry : backtrace) {
            trace_str += fmt::format("\n    {}+0x{:x} ({})", entry.module, entry.offset, entry.name);
        }
        LOG_INFO(Service, "[Nextendo][DIAG] Shutdown fd={} how={} guest backtrace:{}", fd, how,
                 trace_str);
    }

    BuildErrnoResponse(ctx, ShutdownImpl(fd, how));
}

void BSD::Recv(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{:x} len={}", fd, flags, ctx.GetWriteBufferSize());

    ExecuteWork(ctx, RecvWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD::RecvFrom(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{:x} len={} addrlen={}", fd, flags,
              ctx.GetWriteBufferSize(0), ctx.GetWriteBufferSize(1));

    ExecuteWork(ctx, RecvFromWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize(0)),
                         .addr = std::vector<u8>(ctx.GetWriteBufferSize(1)),
                     });
}

void BSD::Send(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{:x} len={}", fd, flags, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD::SendTo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{} len={} addrlen={}", fd, flags,
              ctx.GetReadBufferSize(0), ctx.GetReadBufferSize(1));

    ExecuteWork(ctx, SendToWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(0),
                         .addr = ctx.ReadBuffer(1),
                     });
}

void BSD::Write(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = 0,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD::Read(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetWriteBufferSize());

    // [Nextendo] Real eventfd_read() drains one coalesced counter. This backing socket queues
    // each Write() as its own datagram instead, so a single Read() only pops one of several
    // pending signals, leaving the fd spuriously still-readable. Drain and sum them all here to
    // match real eventfd semantics.
    if (IsFileDescriptorValid(fd) && file_descriptors[fd]->is_eventfd) {
        u64 total = 0;
        s32 drained = 0;
        Errno last_errno = Errno::SUCCESS;
        for (int i = 0; i < 1024; ++i) {
            // [Nextendo] Only the FIRST read may legitimately block -- a guest that calls Read()
            // without polling first is entitled to wait for at least one value, matching real
            // eventfd_read() semantics. Every read after that is purely this loop's own
            // speculative "is there already more queued" check: RecvImpl(fd, 0, ...) doesn't
            // force non-blocking here (no FLAG_MSG_DONTWAIT, and EventFd() never sets the
            // backing socket non-blocking either), so without this guard a drain attempt past
            // the last pending datagram calls a genuinely blocking recv() and hangs whichever
            // BSD worker thread is running it. Confirmed live: this was the last socket-layer
            // event before an ~19s stall, with Recv/RecvMMsg on Splatoon 3's TLS socket never
            // running again for the rest of that connection attempt.
            if (i > 0) {
                std::vector<Network::PollFD> peek_fds{Network::PollFD{
                    .socket = file_descriptors[fd]->socket.get(),
                    .events = Network::PollEvents::In,
                    .revents = Network::PollEvents{},
                }};
                const auto [peek_ret, peek_errno] = Network::Poll(peek_fds, 0);
                if (peek_ret <= 0 || peek_errno != Network::Errno::SUCCESS ||
                    False(peek_fds[0].revents & Network::PollEvents::In)) {
                    break;
                }
            }
            std::vector<u8> chunk(sizeof(u64));
            const auto [chunk_ret, chunk_errno] = RecvImpl(fd, 0, chunk);
            if (chunk_ret != sizeof(u64)) {
                last_errno = chunk_errno;
                break;
            }
            u64 value;
            std::memcpy(&value, chunk.data(), sizeof(value));
            total += value;
            ++drained;
        }

        IPC::ResponseBuilder rb{ctx, 4};
        if (drained > 0) {
            std::vector<u8> message(sizeof(u64));
            std::memcpy(message.data(), &total, sizeof(total));
            ctx.WriteBuffer(message);
            LOG_DEBUG(Service, "[Nextendo] eventfd fd={} Read coalesced {} pending write(s) into {}",
                     fd, drained, total);
            rb.Push(ResultSuccess);
            rb.Push<s32>(sizeof(u64));
            rb.PushEnum(Errno::SUCCESS);
        } else {
            rb.Push(ResultSuccess);
            rb.Push<s32>(-1);
            rb.PushEnum(last_errno);
        }
        return;
    }

    std::vector<u8> message(ctx.GetWriteBufferSize());
    const auto [ret, bsd_errno] = RecvImpl(fd, 0, message);
    ctx.WriteBuffer(message);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::Close(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    BuildErrnoResponse(ctx, CloseImpl(fd));
}

void BSD::DuplicateSocket(HLERequestContext& ctx) {
    struct InputParameters {
        s32 fd;
        u64 reserved;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    struct OutputParameters {
        s32 ret;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x8);

    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    Expected<s32, Errno> res = DuplicateSocketImpl(input.fd);
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .ret = res.value_or(0),
        .bsd_errno = res ? Errno::SUCCESS : res.error(),
    });
}

void BSD::EventFd(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u64 initval = rp.Pop<u64>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. initval={} flags={}", initval, flags);

    // Real eventfd semantics, built out of a UDP socket connected to itself over loopback:
    // Write() adds a datagram (readable/poll-worthy immediately), Read() drains the next one.
    // Games use this as a self-pipe to wake a blocked Poll/Select from another thread (e.g. to
    // interrupt a host's listen loop) -- without a real, pollable fd behind it, that signal goes
    // nowhere and the listener never wakes for anything but its own socket traffic.
    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::MFILE);
        return;
    }

    auto socket = std::make_shared<Network::Socket>();
    Network::Errno net_err = socket->Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                                Network::Protocol::UDP);
    const Network::SockAddrIn loopback{
        .family = Network::Domain::INET,
        .ip = {127, 0, 0, 1},
        .portno = 0,
    };
    if (net_err == Network::Errno::SUCCESS) {
        net_err = socket->Bind(loopback);
    }
    Network::SockAddrIn bound{};
    if (net_err == Network::Errno::SUCCESS) {
        std::tie(bound, net_err) = socket->GetSockName();
    }
    if (net_err == Network::Errno::SUCCESS) {
        bound.ip = loopback.ip;
        net_err = socket->Connect(bound);
    }
    if (net_err != Network::Errno::SUCCESS) {
        LOG_ERROR(Service, "Failed to create eventfd backing socket, errno={}",
                  static_cast<int>(net_err));
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Translate(net_err));
        return;
    }

    file_descriptors[fd] = FileDescriptor{};
    FileDescriptor& descriptor = *file_descriptors[fd];
    descriptor.socket = std::move(socket);
    descriptor.domain = Network::Domain::INET;
    descriptor.type = Network::Type::DGRAM;
    descriptor.protocol = Network::Protocol::UDP;
    descriptor.is_connection_based = true; // enables Write()/Send() without an explicit dest
    descriptor.connected = true;
    descriptor.is_eventfd = true;

    if (initval > 0) {
        const u64 seed = initval;
        descriptor.socket->Send(
            std::span<const u8>{reinterpret_cast<const u8*>(&seed), sizeof(seed)}, 0);
    }

    LOG_INFO(Service, "[Nextendo] New eventfd fd={} initval={}", fd, initval);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(Errno::SUCCESS);
}

void BSD::RegisterClientShared(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called RegisterClientShared");
    IPC::ResponseBuilder rb{ctx, 4}; // Match RegisterClient response style
    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // ret (0 for success)
    rb.Push<s32>(0); // BSD errno (0 for success, consistent with RegisterClient stub)
}

template <typename Work>
void BSD::ExecuteWork(HLERequestContext& ctx, Work work) {
    work.Execute(this);
    work.Response(ctx);
}

std::pair<s32, Errno> BSD::SocketImpl(Domain domain, Type type, Protocol protocol) {
    if (type == Type::SEQPACKET) {
        UNIMPLEMENTED_MSG("SOCK_SEQPACKET errno management");
    } else if (type == Type::RAW && (domain != Domain::INET || protocol != Protocol::ICMP)) {
        UNIMPLEMENTED_MSG("SOCK_RAW errno management");
    }

    [[maybe_unused]] const bool unk_flag = (static_cast<u32>(type) & 0x20000000) != 0;
    UNIMPLEMENTED_IF_MSG(unk_flag, "Unknown flag in type");
    type = static_cast<Type>(static_cast<u32>(type) & ~0x20000000);

    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    file_descriptors[fd] = FileDescriptor{};
    FileDescriptor& descriptor = *file_descriptors[fd];
    // ENONMEM might be thrown here

    auto room_member = room_network.GetRoomMember().lock();
    const bool using_proxy = room_member && room_member->IsConnected();

    LOG_INFO(Service, "New socket fd={} domain={} type={} protocol={} proxy={}",
             fd, domain, type, protocol, using_proxy);

    // Store socket type information for pooling
    descriptor.domain = Translate(domain);
    descriptor.type = Translate(type);
    descriptor.protocol = Translate(protocol);
    descriptor.is_connection_based = IsConnectionBased(type);

    if (Settings::values.airplane_mode.GetValue()) {
        descriptor.socket = std::make_shared<OfflineSocket>();
        descriptor.socket->Initialize(descriptor.domain, descriptor.type, descriptor.protocol);
        LOG_INFO(Service, "Airplane mode: created offline socket fd={}", fd);
    } else if (using_proxy) {
        descriptor.socket = std::make_shared<Network::ProxySocket>(room_network);
        descriptor.socket->Initialize(descriptor.domain, descriptor.type, descriptor.protocol);
        LOG_DEBUG(Service, "Created new ProxySocket for fd={}", fd);
    } else {
        descriptor.socket = std::make_shared<Network::Socket>();
        descriptor.socket->Initialize(descriptor.domain, descriptor.type, descriptor.protocol);
        if (type == Type::DGRAM) {
            // Guest P2P (Pia) traffic can arrive as a burst of several large datagrams within
            // single-digit milliseconds of each other (observed: a friend-island-visit payload
            // delivered as ~5 fragments up to 880 bytes each, back-to-back). The OS default
            // SO_RCVBUF is small enough that a burst like this can silently overflow it before
            // the guest's own poll loop drains it, dropping datagrams with no trace on either
            // side. Request a generous buffer up front; this only widens headroom and cannot
            // change any protocol behavior, so it's safe even if this guess turns out wrong.
            constexpr u32 kGenerousUdpRcvBuf = 1024 * 1024;
            descriptor.socket->SetRcvBuf(kGenerousUdpRcvBuf);
        }
    }

    return {fd, Errno::SUCCESS};
}

bool BSD::PollSetIncludesEventFd(std::span<const u8> read_buffer, s32 nfds) const {
    if (nfds <= 0 || read_buffer.size() < static_cast<size_t>(nfds) * sizeof(PollFD)) {
        return false;
    }
    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));
    for (const PollFD& pollfd : fds) {
        if (pollfd.fd < 0 || pollfd.fd > static_cast<s32>(MAX_FD)) {
            continue;
        }
        const auto& descriptor = file_descriptors[pollfd.fd];
        if (descriptor && descriptor->is_eventfd) {
            return true;
        }
    }
    return false;
}

std::pair<s32, Errno> BSD::PollImpl(std::vector<u8>& write_buffer, std::span<const u8> read_buffer,
                                    s32 nfds, s32 timeout) {
    if (nfds <= 0) {
        // poll(NULL, 0, timeout) is a portable sleep idiom titles use for pacing/backoff
        // between retries. Real poll() actually blocks for the requested duration; honor
        // that here instead of returning instantly, or a title's own retry-count budget
        // burns through in microseconds instead of the real time it was paced for.
        if (timeout > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        }
        // When no entries are provided, -1 is returned with errno zero
        return {-1, Errno::SUCCESS};
    }
    if (read_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }
    if (write_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }

    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));

    // Initialize revents to zero to ensure clean state
    for (PollFD& pollfd : fds) {
        pollfd.revents = PollEvents{};
    }

    if (timeout >= 0) {
        const s64 seconds = timeout / 1000;
        const u64 nanoseconds = 1'000'000 * (static_cast<u64>(timeout) % 1000);

        if (seconds < 0) {
            return {-1, Errno::INVAL};
        }
        if (nanoseconds > 999'999'999) {
            return {-1, Errno::INVAL};
        }
    } else if (timeout != -1) {
        return {-1, Errno::INVAL};
    }

    // [Nextendo] A dead/invalid fd used to fail the WHOLE poll call (return {0, SUCCESS}
    // immediately, discarding every other entry's real state) the instant it was found, even
    // when other fds in the same set -- the eventfd gRPC polls alongside its socket, say -- were
    // genuinely ready. Real poll() reports POLLNVAL on just THAT entry's line and still evaluates
    // the rest. Matches Ryujinx-Nextendo's own fix for this exact bug (ServerBase.cs's
    // IClient.Poll dead-descriptor handling).
    std::vector<bool> is_valid(fds.size(), true);
    bool any_invalid = false;
    for (size_t i = 0; i < fds.size(); ++i) {
        PollFD& pollfd = fds[i];
        ASSERT(False(pollfd.revents));

        if (pollfd.fd > static_cast<s32>(MAX_FD) || pollfd.fd < 0) {
            LOG_ERROR(Service, "File descriptor handle={} is invalid", pollfd.fd);
            is_valid[i] = false;
            any_invalid = true;
            continue;
        }

        const std::optional<FileDescriptor>& descriptor = file_descriptors[pollfd.fd];
        if (!descriptor) {
            LOG_TRACE(Service, "File descriptor handle={} is not allocated", pollfd.fd);
            is_valid[i] = false;
            any_invalid = true;
        }
    }

    // Only valid entries get a real host-side poll; invalid ones are reported directly below.
    std::vector<size_t> valid_indices;
    std::vector<Network::PollFD> host_pollfds;
    for (size_t i = 0; i < fds.size(); ++i) {
        if (!is_valid[i]) {
            continue;
        }
        valid_indices.push_back(i);
        Network::PollFD entry;
        entry.socket = file_descriptors[fds[i].fd]->socket.get();
        entry.events = Translate(fds[i].events);
        entry.revents = Network::PollEvents{};
        // [Nextendo] gRPC polls its wakeup eventfd with a zero event mask (valid POSIX: normally
        // means "only tell me about errors"), but expects readability to be reported anyway once
        // it writes that eventfd. Matches Ryujinx-Nextendo's EventFileDescriptorPollManager fix
        // for the same grpc behavior -- without it this poll never reports ready and grpc's
        // wakeup loop stalls.
        if (entry.events == Network::PollEvents{} && file_descriptors[fds[i].fd]->is_eventfd) {
            entry.events |= Network::PollEvents::In;
        }
        host_pollfds.push_back(entry);
    }

    // host_pollfds.empty() can only happen when every entry was invalid, in which case
    // any_invalid is already true and the POLLNVAL-implies-immediate-return rule below applies
    // -- nothing to host-poll for, so there's no separate empty-but-valid case to wait out here.
    std::pair<s32, Network::Errno> result{0, Network::Errno::SUCCESS};
    if (!host_pollfds.empty()) {
        result = Network::Poll(host_pollfds, any_invalid ? 0 : timeout);
    }

    for (size_t j = 0; j < valid_indices.size(); ++j) {
        const size_t i = valid_indices[j];
        // A TLS backend can decrypt an entire record in one Read(), buffering any plaintext
        // beyond what the caller asked for internally rather than leaving it on the raw
        // socket -- the raw poll() above has no way to see that, so it never reports this fd
        // readable again even though there's already-decrypted data waiting.
        if (False(host_pollfds[j].revents & Network::PollEvents::In) &&
            True(Translate(fds[i].events) & Network::PollEvents::In) &&
            Service::SSL::HasSslPendingData(host_pollfds[j].socket)) {
            host_pollfds[j].revents |= Network::PollEvents::In;
            if (result.first <= 0) {
                result.first = 1;
            }
        }
        fds[i].revents = Translate(host_pollfds[j].revents);
    }

    for (size_t i = 0; i < fds.size(); ++i) {
        if (fds[i].fd < 0 || fds[i].fd > static_cast<s32>(MAX_FD)) {
            continue;
        }
        const auto& d = file_descriptors[fds[i].fd];
        if (d && d->sni_injected) {
            LOG_INFO(Service,
                     "[Nextendo][DIAG] Poll fd={} requested_events={:#x} revents={:#x} timeout={}",
                     fds[i].fd, static_cast<u16>(fds[i].events), static_cast<u16>(fds[i].revents),
                     timeout);
        }
    }

    s32 real_count = 0;
    for (size_t i = 0; i < fds.size(); ++i) {
        if (!is_valid[i]) {
            fds[i].revents = PollEvents::Nval;
        }
        if (fds[i].revents != PollEvents{}) {
            ++real_count;
        }
    }
    std::memcpy(write_buffer.data(), fds.data(), nfds * sizeof(PollFD));

    if (host_pollfds.empty()) {
        return {real_count, Errno::SUCCESS};
    }
    if (result.second != Network::Errno::SUCCESS) {
        return Translate(result);
    }
    return {real_count, Errno::SUCCESS};
}

namespace {
// fd_set is a plain byte array, bit i (LSB-first within each byte) == fd i.
void ExtractFdsFromMask(std::span<const u8> mask, std::vector<s32>& out) {
    for (size_t byte_idx = 0; byte_idx < mask.size(); ++byte_idx) {
        const u8 current = mask[byte_idx];
        for (int bit = 0; bit < 8; ++bit) {
            if (current & (1u << bit)) {
                out.push_back(static_cast<s32>(byte_idx * 8 + bit));
            }
        }
    }
}

void SetFdInMask(std::vector<u8>& mask, s32 fd) {
    const size_t byte_idx = static_cast<size_t>(fd) / 8;
    if (byte_idx < mask.size()) {
        mask[byte_idx] |= static_cast<u8>(1u << (fd % 8));
    }
}
} // Anonymous namespace

std::pair<s32, Errno> BSD::SelectImpl(s32 nfds, s32 timeout, std::span<const u8> read_in,
                                      std::span<const u8> write_in, std::span<const u8> error_in,
                                      std::vector<u8>& read_out, std::vector<u8>& write_out,
                                      std::vector<u8>& error_out) {
    std::ranges::fill(read_out, 0);
    std::ranges::fill(write_out, 0);
    std::ranges::fill(error_out, 0);

    std::vector<s32> read_fds;
    std::vector<s32> write_fds;
    std::vector<s32> error_fds;
    ExtractFdsFromMask(read_in, read_fds);
    ExtractFdsFromMask(write_in, write_fds);
    ExtractFdsFromMask(error_in, error_fds);

    if (nfds <= 0 || (read_fds.empty() && write_fds.empty() && error_fds.empty())) {
        return {0, Errno::SUCCESS};
    }

    // One poll entry per unique fd, requesting whichever of In/Out it was asked about.
    // Err/Hup/Nval come back from the host poll() unconditionally, regardless of what
    // was requested, matching POSIX poll() semantics.
    struct Entry {
        s32 fd;
        Network::PollEvents requested{};
    };
    std::vector<Entry> entries;
    const auto add = [&](const std::vector<s32>& fds, Network::PollEvents event) {
        for (const s32 fd : fds) {
            const auto it =
                std::ranges::find_if(entries, [fd](const Entry& e) { return e.fd == fd; });
            if (it != entries.end()) {
                it->requested |= event;
            } else {
                entries.push_back({fd, event});
            }
        }
    };
    add(read_fds, Network::PollEvents::In);
    add(write_fds, Network::PollEvents::Out);

    std::vector<s32> polled_fds;
    std::vector<Network::PollFD> host_pollfds;
    polled_fds.reserve(entries.size());
    host_pollfds.reserve(entries.size());
    for (const Entry& entry : entries) {
        if (entry.fd < 0 || entry.fd >= static_cast<s32>(MAX_FD) || !file_descriptors[entry.fd] ||
            !file_descriptors[entry.fd]->socket) {
            continue;
        }
        polled_fds.push_back(entry.fd);
        host_pollfds.push_back(Network::PollFD{
            .socket = file_descriptors[entry.fd]->socket.get(),
            .events = entry.requested,
            .revents = Network::PollEvents{},
        });
    }

    const auto [poll_ret, poll_errno] = Translate(Network::Poll(host_pollfds, timeout));
    if (poll_errno != Errno::SUCCESS) {
        return {poll_ret, poll_errno};
    }

    // error_fds only ever reports out-of-band/exceptional conditions; a plain closed/errored
    // socket surfaces through the read or write set it was asked about, same as real select().
    constexpr auto err_like =
        Network::PollEvents::Err | Network::PollEvents::Hup | Network::PollEvents::Nval;
    s32 ready = 0;
    for (size_t i = 0; i < host_pollfds.size(); ++i) {
        const s32 fd = polled_fds[i];
        const Network::PollEvents revents = host_pollfds[i].revents;
        bool counted = false;
        if (True(host_pollfds[i].events & Network::PollEvents::In) &&
            True(revents & (Network::PollEvents::In | err_like))) {
            SetFdInMask(read_out, fd);
            counted = true;
        }
        if (True(host_pollfds[i].events & Network::PollEvents::Out) &&
            True(revents & (Network::PollEvents::Out | err_like))) {
            SetFdInMask(write_out, fd);
            counted = true;
        }
        if (True(revents & (Network::PollEvents::Err | Network::PollEvents::Hup))) {
            SetFdInMask(error_out, fd);
            counted = true;
        }
        if (counted) {
            ++ready;
        }
    }

    return {ready, Errno::SUCCESS};
}

std::pair<s32, Errno> BSD::AcceptImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    auto [result, bsd_errno] = descriptor.socket->Accept();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return {-1, Translate(bsd_errno)};
    }

    file_descriptors[new_fd] = FileDescriptor{};
    FileDescriptor& new_descriptor = *file_descriptors[new_fd];
    new_descriptor.socket = std::move(result.socket);
    new_descriptor.is_connection_based = descriptor.is_connection_based;

    const SockAddrIn guest_addr_in = Translate(result.sockaddr_in);
    PutValue(write_buffer, guest_addr_in);

    return {new_fd, Errno::SUCCESS};
}

Errno BSD::BindImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        LOG_ERROR(Service, "Bind failed: Invalid fd={}", fd);
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    // [Nextendo] A 28-byte buffer here is a real sockaddr_in6 (an IPv6 bind) -- Citron doesn't
    // support IPv6 sockets, and misreading its first 16 bytes as a 4-byte IPv4 address plus 8
    // bytes of the IPv6 payload used to silently corrupt every field read below it and spam
    // assertion failures downstream (network.cpp's TranslateFromSockAddrIn: it only knows
    // Domain::INET). Titles that try IPv6 first and fall back to IPv4 on failure (Splatoon 3's
    // gRPC/NPLN stack does) need a clean, real EAFNOSUPPORT here to take that fallback path
    // properly -- garbage/EINVAL from a corrupted parse instead left the whole connection
    // sequence unable to ever complete or definitively fail, looping forever.
    if (addr.size() != sizeof(SockAddrIn)) {
        LOG_WARNING(Service, "Bind fd={} with unsupported address size={} (IPv6?), returning "
                              "EAFNOSUPPORT", fd, addr.size());
        return Errno::AFNOSUPPORT;
    }
    auto addr_in = GetValue<SockAddrIn>(addr);

    LOG_INFO(Service, "Bind fd={} to {}:{}", fd, Network::IPv4AddressToRedactedString(addr_in.ip),
             addr_in.portno);

    FileDescriptor& descriptor = *file_descriptors[fd];
    if (descriptor.type == Network::Type::DGRAM && addr_in.portno > 0) {
        auto [parked, queued] = TakeParkedUdpSocket(addr_in.portno);
        if (parked) {
            LOG_INFO(Service,
                     "[Nextendo] Reusing parked UDP socket for port {} ({} buffered datagram(s))",
                     addr_in.portno, queued.size());
            // Close the displaced socket, or every adopt leaks a host descriptor.
            if (descriptor.socket) {
                descriptor.socket->Close();
            }
            descriptor.socket = std::move(parked);
            descriptor.bound_port = addr_in.portno;
            // Datagrams the drain thread caught while this socket sat parked go first, so
            // RecvFrom serves them before anything read live off the socket from here on.
            for (auto& datagram : queued) {
                descriptor.pending_datagrams.push_back(std::move(datagram));
            }
            return Errno::SUCCESS;
        }
        descriptor.bound_port = addr_in.portno;
    }

    const auto result = Translate(file_descriptors[fd]->socket->Bind(Translate(addr_in)));
    if (result != Errno::SUCCESS) {
        LOG_ERROR(Service, "Bind fd={} failed with errno={}", fd, static_cast<int>(result));
    }
    return result;
}

Errno BSD::ConnectImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        LOG_ERROR(Service, "Connect failed: Invalid fd={}", fd);
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;
    if (Settings::values.airplane_mode.GetValue()) {
        return Errno::CONNREFUSED;
    }

    // [Nextendo] See BindImpl's comment on the same size check -- an IPv6 sockaddr here hits
    // the same unsupported-address-family case, and needs the same clean rejection rather than
    // a misparsed connect target.
    if (addr.size() != sizeof(SockAddrIn)) {
        LOG_WARNING(Service, "Connect fd={} with unsupported address size={} (IPv6?), returning "
                              "EAFNOSUPPORT", fd, addr.size());
        return Errno::AFNOSUPPORT;
    }
    auto addr_in = GetValue<SockAddrIn>(addr);
    auto translated_addr = Translate(addr_in);

    // [Nextendo] Splatoon 3's gRPC stack loses the resolved address in its own connection
    // plumbing and calls connect() with a zeroed IP, keeping only the port it originally
    // resolved for. Recover it from what GetAddrInfoRequestImpl recorded for that exact
    // port. Only ever populated for a Nextendo-redirected hostname's own port, so this can't
    // misfire on a P2P socket targeting another console's port -- that port was never part of
    // a redirect, so the lookup below returns nothing and the address is left untouched.
    static constexpr std::array<u8, 4> zero_addr{0, 0, 0, 0};
    if (addr_in.ip == zero_addr && translated_addr.portno != 0) {
        if (const auto recovered = GetLastIpForPort(translated_addr.portno)) {
            LOG_INFO(Service,
                     "[Nextendo] Connect fd={} address was lost (zeroed), recovered {} for "
                     "port {} from an earlier redirected resolution",
                     fd, Network::IPv4AddressToRedactedString(*recovered),
                     translated_addr.portno);
            translated_addr.ip = *recovered;
        }
    }

    LOG_INFO(Service, "Connect fd={} to {}:{}", fd,
             Network::IPv4AddressToRedactedString(addr_in.ip), translated_addr.portno);

    const auto result = Translate(file_descriptors[fd]->socket->Connect(translated_addr));
    if (result == Errno::SUCCESS || result == Errno::INPROGRESS) {
        file_descriptors[fd]->connected = true;
    }
    if (result != Errno::SUCCESS) {
        LOG_ERROR(Service, "Connect fd={} failed with errno={}", fd, static_cast<int>(result));
    } else {
        LOG_INFO(Service, "Connect fd={} succeeded", fd);
        // [Nextendo][DIAG] See connect_success_time's declaration comment in bsd.h.
        file_descriptors[fd]->connect_success_time = std::chrono::steady_clock::now();
    }
    return result;
}

Errno BSD::GetPeerNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetPeerName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    ASSERT(write_buffer.size() >= sizeof(guest_addrin));
    write_buffer.resize(sizeof(guest_addrin));
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD::GetSockNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetSockName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    ASSERT(write_buffer.size() >= sizeof(guest_addrin));
    write_buffer.resize(sizeof(guest_addrin));
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD::ListenImpl(s32 fd, s32 backlog) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;
    return Translate(file_descriptors[fd]->socket->Listen(backlog));
}

std::pair<s32, Errno> BSD::FcntlImpl(s32 fd, FcntlCmd cmd, s32 arg) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket)
        return {-1, Errno::BADF};

    FileDescriptor& descriptor = *file_descriptors[fd];

    switch (cmd) {
    case FcntlCmd::GETFL:
        ASSERT(arg == 0);
        return {descriptor.flags, Errno::SUCCESS};
    case FcntlCmd::SETFL: {
        const bool enable = (arg & Network::FLAG_O_NONBLOCK) != 0;
        const Errno bsd_errno = Translate(descriptor.socket->SetNonBlock(enable));
        if (bsd_errno != Errno::SUCCESS) {
            return {-1, bsd_errno};
        }
        descriptor.flags = arg;
        return {0, Errno::SUCCESS};
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented cmd={}", cmd);
        return {-1, Errno::SUCCESS};
    }
}

Errno BSD::GetSockOptImpl(s32 fd, u32 level, OptName optname, std::vector<u8>& optval) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    // [Nextendo] IPPROTO_TCP / TCP_NODELAY. A portable networking stack (Splatoon 3's
    // gRPC/NintendoSDK_NPLN stack does this) commonly disables Nagle's algorithm on connect
    // and reads it back to confirm before trusting the socket -- the generic INVAL below used
    // to fail that check and made the game abandon an otherwise-fine socket, retrying the
    // whole connection sequence from scratch forever. See SocketBase::GetNoDelay's declaration
    // comment in internal_network/sockets.h.
    if (level == 6 && static_cast<u32>(optname) == 1) {
        auto [enabled, getsockopt_err] = file_descriptors[fd]->socket->GetNoDelay();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            ASSERT_OR_EXECUTE_MSG(
                optval.size() == sizeof(u32), { return Errno::INVAL; },
                "Incorrect getsockopt option size");
            optval.resize(sizeof(u32));
            PutValue(optval, static_cast<u32>(enabled ? 1 : 0));
        }
        return Translate(getsockopt_err);
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        // [Nextendo] SetSockOptImpl already tolerates any level it doesn't specifically implement
        // (returns SUCCESS, matching Ryujinx-Nextendo's own generic tolerant fallback) -- this must
        // do the same rather than INVAL, or a title that sets an option at some other level (e.g.
        // IPPROTO_IPV6 on a dual-stack socket) and reads it straight back to confirm sees a set-ok/
        // get-fails mismatch. Ryujinx-Nextendo's own comment on this exact asymmetry: "grpc-core
        // sets TCP_NODELAY / SO_REUSEADDR / etc. and reads them straight back; a set-ok / get-
        // EOPNOTSUPP mismatch made it close before connect." Echo back zeroed bytes of the
        // requested size rather than failing outright.
        LOG_WARNING(Service, "(STUBBED) Unknown getsockopt level={}, echoing zeroed value", level);
        std::ranges::fill(optval, 0);
        return Errno::SUCCESS;
    }

    Network::SocketBase* const socket = file_descriptors[fd]->socket.get();

    // [Nextendo] See GetReuseAddr's declaration comment in internal_network/sockets.h --
    // REUSEADDR/KEEPALIVE/BROADCAST used to have a setter but no matching getter, so a title
    // that verifies one of these right after setting it (Splatoon 3's gRPC/NintendoSDK_NPLN
    // stack does) always got INVAL back and abandoned an otherwise-fine socket.
    auto write_bool_opt = [&](std::pair<bool, Network::Errno> result) -> Errno {
        auto [enabled, getsockopt_err] = result;
        if (getsockopt_err == Network::Errno::SUCCESS) {
            ASSERT_OR_EXECUTE_MSG(
                optval.size() == sizeof(u32), { return Errno::INVAL; },
                "Incorrect getsockopt option size");
            optval.resize(sizeof(u32));
            PutValue(optval, static_cast<u32>(enabled ? 1 : 0));
        }
        return Translate(getsockopt_err);
    };

    // [Nextendo] optname=0x80000001 is never actually applied to the real socket (see
    // SetSockOptImpl) -- echo back whatever bytes were "set", matching Ryujinx-Nextendo's
    // _feignedSockOpts behavior, instead of querying the real (untouched) socket's linger state.
    if (static_cast<u32>(optname) == 0x80000001) {
        const auto& descriptor = *file_descriptors[fd];
        optval.resize(8);
        if (descriptor.vendor_linger_feigned) {
            std::memcpy(optval.data(), descriptor.vendor_linger_feigned->data(), 8);
        } else {
            std::ranges::fill(optval, 0);
        }
        return Errno::SUCCESS;
    }

    // [Nextendo] SO_LINGER. A title that sets this and reads it back to confirm it took
    // previously got INVAL, same failure class as the TCP_NODELAY/REUSEADDR gaps above.
    if (optname == OptName::LINGER) {
        auto [onoff, linger, getsockopt_err] = socket->GetLinger();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            ASSERT_OR_EXECUTE_MSG(
                optval.size() == sizeof(Linger), { return Errno::INVAL; },
                "Incorrect getsockopt option size");
            optval.resize(sizeof(Linger));
            PutValue(optval, Linger{.onoff = onoff ? 1u : 0u, .linger = linger});
        }
        return Translate(getsockopt_err);
    }

    switch (optname) {
    case OptName::ERROR_: {
        auto [pending_err, getsockopt_err] = socket->GetPendingError();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            Errno translated_pending_err = Translate(pending_err);
            ASSERT_OR_EXECUTE_MSG(
                optval.size() == sizeof(Errno), { return Errno::INVAL; },
                "Incorrect getsockopt option size");
            optval.resize(sizeof(Errno));
            PutValue(optval, translated_pending_err);
        }
        return Translate(getsockopt_err);
    }
    case OptName::REUSEADDR:
        return write_bool_opt(socket->GetReuseAddr());
    case OptName::KEEPALIVE:
        return write_bool_opt(socket->GetKeepAlive());
    case OptName::BROADCAST:
        return write_bool_opt(socket->GetBroadcast());
    default:
        LOG_WARNING(Service, "(STUBBED) Unimplemented optname={} (0x{:x}), returning INVAL",
                    static_cast<u32>(optname), static_cast<u32>(optname));
        return Errno::INVAL;
    }
}

Errno BSD::SetSockOptImpl(s32 fd, u32 level, OptName optname, std::span<const u8> optval) {
    if (!IsFileDescriptorValid(fd))
        return Errno::BADF;
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    // [Nextendo] IPPROTO_TCP / TCP_NODELAY -- see the matching comment in GetSockOptImpl.
    if (level == 6 && static_cast<u32>(optname) == 1) {
        if (optval.size() != sizeof(u32)) {
            LOG_WARNING(Service, "TCP_NODELAY optval size mismatch: expected {}, got {}",
                        sizeof(u32), optval.size());
            return Errno::INVAL;
        }
        const auto value = GetValue<u32>(optval);
        return Translate(file_descriptors[fd]->socket->SetNoDelay(value != 0));
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        LOG_WARNING(Service, "(STUBBED) Unknown setsockopt level={}, returning SUCCESS for compatibility", level);
        return Errno::SUCCESS;
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    Network::SocketBase* const socket = descriptor.socket.get();

    // [Nextendo] optname=0x80000001 (undocumented in public libnx headers) is Splatoon 3's
    // NPLN/gRPC stack's own vendor-private setsockopt, called once right after every connect
    // with an 8-byte payload. A prior guess treated this as Nintendo's proprietary spelling of
    // SO_LINGER and applied it for real (onoff=1, 2s timeout) -- but Ryujinx-Nextendo's own
    // working reference implementation (ManagedSocket.SetSocketOption) does the opposite: an
    // option that doesn't validate against its known BsdSocketOption table is fed straight to
    // its generic tolerant fallback, which remembers the bytes for getsockopt to echo back but
    // never calls the real socket's SetSocketOption at all. Match that here: stash the bytes so
    // GetSockOptImpl can echo them back, but never touch the real socket's linger state.
    if (static_cast<u32>(optname) == 0x80000001) {
        std::array<u8, 8> stored{};
        std::memcpy(stored.data(), optval.data(), std::min(optval.size(), stored.size()));
        descriptor.vendor_linger_feigned = stored;
        return Errno::SUCCESS;
    }

    if (optname == OptName::LINGER) {
        if (optval.size() != sizeof(Linger)) {
            LOG_WARNING(Service, "LINGER optval size mismatch: expected {}, got {}", sizeof(Linger),
                        optval.size());
            return Errno::INVAL;
        }
        auto linger = GetValue<Linger>(optval);
        if (linger.onoff != 0 && linger.onoff != 1) {
            LOG_WARNING(Service, "Invalid LINGER onoff value: {}", linger.onoff);
            return Errno::INVAL;
        }
        return Translate(socket->SetLinger(linger.onoff != 0, linger.linger));
    }

    // [Nextendo] The old code gated ALL of setsockopt on "optval is exactly 4 bytes",
    // unconditionally returning EINVAL for any option shaped differently -- BEFORE ever checking
    // whether the option is even one Citron recognizes. Every other unimplemented optname in this
    // function is a soft "(STUBBED) ... returning SUCCESS" no-op; an option Citron simply hasn't
    // implemented shouldn't fail any harder just because its payload also happens not to be 4
    // bytes. Observed live: Splatoon 3's gRPC stack calls setsockopt with optname=0x80000001
    // optval.size()=8 on every single connection (both the abandoned IPv6 dual-stack attempt and
    // the real IPv4 socket) -- not a value in Citron's or libnx's known SO_* numbering, so its
    // exact meaning is unclear, but failing it outright when an unrecognized 4-byte option next
    // to it would have been silently accepted is an inconsistency with no justification. Route
    // unrecognized optnames to the same graceful stub regardless of size; only the options this
    // function actually implements still require their expected 4-byte u32 payload.
    switch (optname) {
    case OptName::REUSEADDR:
    case OptName::KEEPALIVE:
    case OptName::SNDBUF:
    case OptName::RCVBUF:
    case OptName::SNDTIMEO:
    case OptName::RCVTIMEO:
    case OptName::NOSIGPIPE:
        break;
    default:
        if (static_cast<u32>(optname) != 0x200 && optname != OptName::BROADCAST) {
            LOG_WARNING(Service,
                        "(STUBBED) Unimplemented optname={} (0x{:x}) optlen={}, "
                        "returning SUCCESS for compatibility",
                        static_cast<u32>(optname), static_cast<u32>(optname), optval.size());
            return Errno::SUCCESS;
        }
        break;
    }

    if (optval.size() != sizeof(u32)) {
        LOG_WARNING(Service, "optval size mismatch: expected {}, got {} for optname={}", sizeof(u32),
                    optval.size(), static_cast<u32>(optname));
        return Errno::INVAL;
    }
    auto value = GetValue<u32>(optval);

    if (static_cast<u32>(optname) == 0x200 || optname == OptName::BROADCAST) {
        socket->SetBroadcast(value != 0);
        return Errno::SUCCESS;
    }

    switch (optname) {
    case OptName::REUSEADDR:
        if (value != 0 && value != 1) {
            LOG_WARNING(Service, "Invalid REUSEADDR value: {}", value);
            return Errno::INVAL;
        }
        return Translate(socket->SetReuseAddr(value != 0));
    case OptName::KEEPALIVE:
        if (value != 0 && value != 1) {
            LOG_WARNING(Service, "Invalid KEEPALIVE value: {}", value);
            return Errno::INVAL;
        }
        return Translate(socket->SetKeepAlive(value != 0));
    case OptName::SNDBUF:
        return Translate(socket->SetSndBuf(value));
    case OptName::RCVBUF:
        return Translate(socket->SetRcvBuf(value));
    case OptName::SNDTIMEO:
        return Translate(socket->SetSndTimeo(value));
    case OptName::RCVTIMEO:
        return Translate(socket->SetRcvTimeo(value));
    case OptName::NOSIGPIPE:
        LOG_WARNING(Service, "(STUBBED) setting NOSIGPIPE to {}", value);
        return Errno::SUCCESS;
    default:
        // Unreachable: non-4-byte-payload unknown optnames already returned above, and every
        // recognized case is handled explicitly.
        return Errno::SUCCESS;
    }
}

Errno BSD::ShutdownImpl(s32 fd, s32 how) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    // [Nextendo][DIAG] How long did this connection actually live before the guest gave up on
    // it? Logged unconditionally (not gated on sni_injected/deadline-watch) so it's visible on
    // whichever fd actually calls Shutdown, checking for a correlation with a round-number
    // wall-clock deadline (grpc-core's deadline_filter is wall-clock-based).
    if (const auto& t0 = file_descriptors[fd]->connect_success_time) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - *t0)
                                     .count();
        LOG_INFO(Service,
                 "[Nextendo][DIAG] Shutdown fd={} how={} -- {}ms since Connect() succeeded on "
                 "this fd",
                 fd, how, elapsed_ms);
    }

    const Network::ShutdownHow host_how = Translate(static_cast<ShutdownHow>(how));
    return Translate(file_descriptors[fd]->socket->Shutdown(host_how));
}

std::pair<s32, Errno> BSD::RecvImpl(s32 fd, u32 flags, std::vector<u8>& message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    if (Settings::values.airplane_mode.GetValue()) {
        return {-1, Errno::AGAIN};
    }
    // A connect()ed UDP socket (e.g. a P2P client dialing a single host station) supports
    // plain recv() same as TCP; only an unconnected DGRAM socket needs an explicit peer.
    if (!descriptor.is_connection_based && !descriptor.connected) {
        return {-1, Errno::AGAIN};
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    auto [ret, bsd_errno] = Translate(descriptor.socket->Recv(flags, message));

    if (descriptor.sni_injected) {
        LOG_INFO(Service, "[Nextendo][DIAG] Recv fd={} requested={} ret={} errno={}", fd,
                 message.size(), ret, static_cast<int>(bsd_errno));
    }

    // [Nextendo] awaiting_reply is meant as a one-shot grace period for the *next* would-block
    // recv() right after a send -- but it was only ever consumed in the AGAIN branch below. If
    // that next recv() instead succeeds immediately (the common case: a whole TLS flight already
    // sitting in the kernel buffer, e.g. ServerHello+Certificate+ServerKeyExchange+ServerHelloDone
    // arriving as one read), the flag survived untouched and stayed armed for whatever recv() came
    // *after* that -- a routine "anything else?" follow-up call with genuinely nothing pending --
    // forcing an unrelated, fully wasted 250ms block on a thread the guest's own gRPC deadline is
    // timing against. Measured directly: this exact leak burned 250ms of a 731ms connect-to-
    // shutdown budget on one connection, right where a working capture (Ryujinx-Nextendo, same
    // server, same handshake) completes the whole sequence in ~350ms. A successful recv already
    // means the wait condition this flag exists for has been satisfied -- clear it here so it
    // can't leak into an unrelated later call.
    if (ret > 0) {
        descriptor.awaiting_reply = false;
    }

    // [Nextendo] Splatoon 3's gRPC stack sends its TLS ClientKeyExchange/ChangeCipherSpec/
    // Finished flight, does exactly ONE opportunistic non-blocking recv() a fraction of a
    // millisecond later, and -- finding nothing, since no real network reply can possibly
    // exist yet -- immediately abandons the connection and restarts the whole handshake from
    // scratch. Confirmed against the real Nextendo server directly (a plain TLS client gets a
    // full handshake, ChangeCipherSpec+Finished included, in ~200ms round trip): the server is
    // never given a chance to answer before the game gives up. Ryujinx-Nextendo hit the same
    // shape of bug for this exact gRPC connection at the poll layer (IClient.cs's eventfd/socket
    // accumulation and drain fixes) and resolved it by making the host side deterministic
    // instead of racing the guest's own overly-eager give-up logic -- same principle as this
    // socket's Connect() fix. Mirror that here: only for a socket that had its SNI substituted
    // (i.e. only ever an actual Nextendo redirect, never an unrelated title's own socket), give
    // a would-block recv() one bounded blocking wait for real data before honoring it, so the
    // server's already-in-flight reply has an actual chance to arrive first.
    //
    if (bsd_errno == Errno::AGAIN && descriptor.sni_injected && descriptor.awaiting_reply) {
        descriptor.awaiting_reply = false;
        std::vector<Network::PollFD> wait_fds{Network::PollFD{
            .socket = descriptor.socket.get(),
            .events = Network::PollEvents::In,
            .revents = Network::PollEvents{},
        }};
        // [Nextendo] Was 800ms; a real reply here measures ~200ms round trip (see this block's
        // opening comment), and a working Ryujinx-Nextendo capture completes its entire
        // handshake-to-first-real-request sequence in well under 350ms total. Splatoon 3's gRPC
        // call carries its own deadline (embedded grpc-core deadline_filter.cc) computed from
        // wall-clock time since the call started -- every millisecond this wait burns without a
        // real reply pending eats directly into that budget before the guest can even send its
        // first real request. 250ms still comfortably covers the measured real round trip with
        // margin, without spending 4x that on a wait that's usually going to time out anyway.
        constexpr s32 handshake_reply_wait_ms = 250;
        const auto wait_start = std::chrono::steady_clock::now();
        const auto [poll_ret, poll_errno] = Network::Poll(wait_fds, handshake_reply_wait_ms);
        const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - wait_start)
                                 .count();
        if (poll_ret > 0 && poll_errno == Network::Errno::SUCCESS &&
            True(wait_fds[0].revents & Network::PollEvents::In)) {
            std::tie(ret, bsd_errno) = Translate(descriptor.socket->Recv(flags, message));
        }
        LOG_INFO(Service,
                 "[Nextendo][DIAG] Recv fd={} grace-wait took {}ms poll_ret={} -> ret={} errno={}",
                 fd, wait_ms, poll_ret, ret, static_cast<int>(bsd_errno));
    }

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD::RecvFromImpl(s32 fd, u32 flags, std::vector<u8>& message,
                                        std::vector<u8>& addr) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    if (Settings::values.airplane_mode.GetValue()) {
        addr.clear();
        return {-1, Errno::AGAIN};
    }

    Network::SockAddrIn addr_in{};
    Network::SockAddrIn* p_addr_in = nullptr;
    if (descriptor.is_connection_based) {
        // Connection based file descriptors (e.g. TCP) zero addr
        addr.clear();
    } else {
        // Datagram (UDP): receive the sender's address. (Previously this path returned
        // AGAIN unconditionally, which silently broke all UDP recvfrom/MSG_PEEK.)
        p_addr_in = &addr_in;
    }

    // Serve anything a parked socket's drain thread buffered for this fd before touching the
    // live socket, oldest first — otherwise a reused parked socket would silently skip straight
    // past data that arrived during the close/park/rebind gap.
    if (!descriptor.pending_datagrams.empty()) {
        auto [buffered_data, buffered_addr] = std::move(descriptor.pending_datagrams.front());
        descriptor.pending_datagrams.pop_front();

        const s32 ret = static_cast<s32>(std::min(message.size(), buffered_data.size()));
        std::copy_n(buffered_data.begin(), ret, message.begin());

        if (p_addr_in) {
            ASSERT(addr.size() == sizeof(SockAddrIn));
            PutValue(addr, Translate(buffered_addr));
            LOG_DEBUG(Service, "RecvFrom fd={} <- {}:{} len={} (buffered) {}", fd,
                      Network::IPv4AddressToRedactedString(buffered_addr.ip),
                      buffered_addr.portno, ret,
                      DescribePrudpLite(
                          std::span<const u8>{message.data(), static_cast<size_t>(ret)}));
        }
        return {ret, Errno::SUCCESS};
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    auto [ret, bsd_errno] = Translate(descriptor.socket->RecvFrom(flags, message, p_addr_in));

    // P2P shares one socket across every peer, so one unreachable peer must not read as the
    // network dropping. The failed call consumed the queued error; take the next datagram.
    if (!descriptor.is_connection_based) {
        for (int attempt = 0; attempt < 16 && IsTransientDatagramError(bsd_errno); ++attempt) {
            LOG_WARNING(Service, "Discarding queued ICMP error on fd={} errno={}", fd,
                        static_cast<int>(bsd_errno));
            std::tie(ret, bsd_errno) =
                Translate(descriptor.socket->RecvFrom(flags, message, p_addr_in));
        }
    }

    if (bsd_errno != Errno::SUCCESS && bsd_errno != Errno::AGAIN) {
        LOG_WARNING(Service, "RecvFrom fd={} failed with errno={}", fd,
                    static_cast<int>(bsd_errno));
    }

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    if (p_addr_in) {
        if (ret < 0) {
            addr.clear();
        } else {
            ASSERT(addr.size() == sizeof(SockAddrIn));
            const SockAddrIn result = Translate(addr_in);
            PutValue(addr, result);
            LOG_DEBUG(Service, "RecvFrom fd={} <- {}:{} len={} {}", fd,
                      Network::IPv4AddressToRedactedString(addr_in.ip), addr_in.portno, ret,
                      DescribePrudpLite(std::span<const u8>{message.data(),
                                                            static_cast<size_t>(std::max(ret, 0))}));

            // nncs reply is 4x u32 BE: [type][ext port][ext ip][server ip]. Remember the ext
            // ip so nextendo_nat_rewrite.cpp can fix up ReplaceURL's stale station address.
            if (ret == 16 && (addr_in.portno == 10025 || addr_in.portno == 10125)) {
                Common::NextendoNat::SetObservedExternalIp(
                    {message[8], message[9], message[10], message[11]});
            }
        }
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD::SendImpl(s32 fd, u32 flags, std::span<const u8> message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket)
        return {-1, Errno::BADF};
    if (Settings::values.airplane_mode.GetValue()) {
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }
    FileDescriptor& descriptor = *file_descriptors[fd];
    // Same as RecvImpl: a connect()ed UDP socket needs plain send() to actually reach the
    // peer, not a silently-dropped no-op -- that starved P2P clients dialing a host station.
    if (!descriptor.is_connection_based && !descriptor.connected) {
        LOG_DEBUG(Service, "Dropping datagram send without destination fd={}", fd);
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }

    std::span<const u8> send_buf = message;
    std::vector<u8> injected_buf;
    // First ClientHello only; a later handshake record must not be rewritten mid-stream.
    if (!descriptor.sni_injected && message.size() > 5 && message[0] == 0x16 &&
        message[5] == 0x01) {
        descriptor.sni_injected = true;
        auto [peer_addr, err] = descriptor.socket->GetPeerName();
        if (err == Network::Errno::SUCCESS) {
            std::string ip_str = Network::IPv4AddressToString(peer_addr.ip);
            std::string host = Service::Sockets::GetLastHostForIp(ip_str);
            if (!host.empty() && TryInjectTlsSni(message, host, injected_buf)) {
                LOG_INFO(Service, "[Nextendo] Injected SNI extension '{}' into TLS ClientHello for BSD socket fd={}", host, fd);
                send_buf = injected_buf;
            }
        }
    }

    auto [sent_bytes, err] = descriptor.socket->Send(send_buf, flags);
    if (err == Network::Errno::SUCCESS && !injected_buf.empty()) {
        sent_bytes = static_cast<s32>(message.size());
    }
    if (err == Network::Errno::SUCCESS && descriptor.sni_injected) {
        descriptor.awaiting_reply = true;
    }
    if (descriptor.sni_injected) {
        LOG_INFO(Service, "[Nextendo][DIAG] Send fd={} len={} sent={} errno={} first_byte=0x{:02x}",
                 fd, send_buf.size(), sent_bytes, static_cast<int>(Translate(err)),
                 send_buf.empty() ? 0 : send_buf[0]);
    }
    if (err == Network::Errno::SUCCESS && descriptor.is_eventfd) {
        // [Nextendo] Wake any Poll() this game deferred waiting on this eventfd. See
        // sockets.h's SetBsdDeferralEvent declaration comment.
        if (Kernel::KEvent* deferral_event = GetBsdDeferralEvent()) {
            deferral_event->Signal();
        }
    }
    return Translate(std::make_pair(sent_bytes, err));
}

std::pair<s32, Errno> BSD::SendToImpl(s32 fd, u32 flags, std::span<const u8> message,
                                      std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket)
        return {-1, Errno::BADF};
    if (Settings::values.airplane_mode.GetValue()) {
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    // For datagram sockets (UDP), a destination address is required
    if (!descriptor.is_connection_based && addr.empty()) {
        LOG_DEBUG(Service, "Dropping datagram sendto without destination fd={}", fd);
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }

    Network::SockAddrIn addr_in;
    Network::SockAddrIn* p_addr_in = nullptr;
    if (!addr.empty()) {
        ASSERT(addr.size() == sizeof(SockAddrIn));
        auto guest_addr_in = GetValue<SockAddrIn>(addr);
        addr_in = Translate(guest_addr_in);
        p_addr_in = &addr_in;
    }

    if (!descriptor.is_connection_based && p_addr_in) {
        LOG_DEBUG(Service, "SendTo fd={} -> {}:{} len={} {}", fd,
                  Network::IPv4AddressToRedactedString(p_addr_in->ip), p_addr_in->portno,
                  message.size(), DescribePrudpLite(message));
    }

    return Translate(file_descriptors[fd]->socket->SendTo(flags, message, p_addr_in));
}

Errno BSD::CloseImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    std::shared_ptr<Network::SocketBase> socket_to_close;
    u16 bound_port = 0;
    bool is_udp = false;
    bool was_connected = false;

    {
        std::lock_guard lock(fd_table_mutex);
        if (!file_descriptors[fd]->socket)
            return Errno::BADF;
        socket_to_close = file_descriptors[fd]->socket;
        bound_port = file_descriptors[fd]->bound_port;
        is_udp = (file_descriptors[fd]->type == Network::Type::DGRAM);
        was_connected = file_descriptors[fd]->connected;
        file_descriptors[fd].reset();
    }

    // Connected means one peer, so closing it is a real teardown, not the probe/play port swap.
    if (is_udp && bound_port > 0 && !was_connected) {
        if (ParkUdpSocket(socket_to_close, bound_port)) {
            LOG_INFO(Service, "[Nextendo] Parking UDP socket fd={} bound to port {}", fd,
                     bound_port);
            return Errno::SUCCESS;
        }
    }

    // [Nextendo] See DeferredCloseTcpSocket's declaration comment. Only for TCP sockets that
    // were actually connected to a peer -- an unconnected/never-dialed socket has no peer that
    // could still have a reply in flight, so there's nothing to protect by delaying its close.
    if (!is_udp && was_connected) {
        LOG_DEBUG(Service, "[Nextendo] Deferring real close of TCP socket fd={} by {}ms", fd,
                  TCP_CLOSE_GRACE.count());
        DeferredCloseTcpSocket(std::move(socket_to_close));
        return Errno::SUCCESS;
    }

    const Errno bsd_errno = Translate(socket_to_close->Close());
    LOG_INFO(Service, "Close socket fd={}", fd);

    return bsd_errno;
}

Expected<s32, Errno> BSD::DuplicateSocketImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Unexpected(Errno::BADF);
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return Unexpected(Errno::MFILE);
    }

    file_descriptors[new_fd] = file_descriptors[fd];
    return new_fd;
}

std::optional<std::shared_ptr<Network::SocketBase>> BSD::GetSocket(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return std::nullopt;
    }
    return file_descriptors[fd]->socket;
}

s32 BSD::FindFreeFileDescriptorHandle() noexcept {
    for (s32 fd = 0; fd < static_cast<s32>(file_descriptors.size()); ++fd) {
        if (!file_descriptors[fd]) {
            return fd;
        }
    }
    return -1;
}

bool BSD::IsFileDescriptorValid(s32 fd) const noexcept {
    if (fd > static_cast<s32>(MAX_FD) || fd < 0) {
        LOG_ERROR(Service, "Invalid file descriptor handle={}", fd);
        return false;
    }
    if (!file_descriptors[fd]) {
        LOG_ERROR(Service, "File descriptor handle={} is not allocated", fd);
        return false;
    }
    return true;
}

void BSD::BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept {
    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD::OnProxyPacketReceived(const Network::ProxyPacket& packet) {
    // Lock the table so CloseImpl doesn't delete a socket while we are iterating
    std::lock_guard lock(fd_table_mutex);

    // We must ensure we only deliver the packet ONCE
    std::vector<Network::SocketBase*> processed_sockets;

    for (auto& optional_desc : file_descriptors) {
        if (optional_desc.has_value() && optional_desc->socket) {
            Network::SocketBase* socket_ptr = optional_desc->socket.get();

            // If we haven't given this specific socket the packet yet...
            if (std::find(processed_sockets.begin(), processed_sockets.end(), socket_ptr) == processed_sockets.end()) {
                socket_ptr->HandleProxyPacket(packet);
                processed_sockets.push_back(socket_ptr);
            }
        }
    }
}

BSD::BSD(Core::System& system_, const char* name)
    : ServiceFramework{system_, name}, room_network{system_.GetRoomNetwork()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSD::RegisterClient, "RegisterClient"},
        {1, &BSD::StartMonitoring, "StartMonitoring"},
        {2, &BSD::Socket, "Socket"},
        {3, &BSD::SocketExempt, "SocketExempt"},
        {4, &BSD::Open, "Open"},
        {5, &BSD::Select, "Select"},
        {6, &BSD::Poll, "Poll"},
        {7, &BSD::Sysctl, "Sysctl"},
        {8, &BSD::Recv, "Recv"},
        {9, &BSD::RecvFrom, "RecvFrom"},
        {10, &BSD::Send, "Send"},
        {11, &BSD::SendTo, "SendTo"},
        {12, &BSD::Accept, "Accept"},
        {13, &BSD::Bind, "Bind"},
        {14, &BSD::Connect, "Connect"},
        {15, &BSD::GetPeerName, "GetPeerName"},
        {16, &BSD::GetSockName, "GetSockName"},
        {17, &BSD::GetSockOpt, "GetSockOpt"},
        {18, &BSD::Listen, "Listen"},
        {19, &BSD::Ioctl, "Ioctl"},
        {20, &BSD::Fcntl, "Fcntl"},
        {21, &BSD::SetSockOpt, "SetSockOpt"},
        {22, &BSD::Shutdown, "Shutdown"},
        {23, &BSD::ShutdownAllSockets, "ShutdownAllSockets"},
        {24, &BSD::Write, "Write"},
        {25, &BSD::Read, "Read"},
        {26, &BSD::Close, "Close"},
        {27, &BSD::DuplicateSocket, "DuplicateSocket"},
        {28, &BSD::GetResourceStatistics, "GetResourceStatistics"},
        {29, &BSD::RecvMMsg, "RecvMMsg"},
        {30, &BSD::SendMMsg, "SendMMsg"},
        {31, &BSD::EventFd, "EventFd"},
        {32, &BSD::RegisterResourceStatisticsName, "RegisterResourceStatisticsName"},
        {33, &BSD::RegisterClientShared, "RegisterClientShared"},
        {34, &BSD::GetSocketStatistics, "GetSocketStatistics"},
        {35, &BSD::NifIoctl, "NifIoctl"},
        {36, &BSD::Unknown36, "Unknown36"},
        {37, &BSD::Unknown37, "Unknown37"},
        {38, &BSD::Unknown38, "Unknown38"},
        {39, &BSD::Unknown39, "Unknown39"},
        {40, &BSD::Unknown40, "Unknown40"},
        {200, &BSD::SetThreadCoreMask, "SetThreadCoreMask"},
        {201, &BSD::GetThreadCoreMask, "GetThreadCoreMask"},
    };
    // clang-format on

    RegisterHandlers(functions);

    if (auto room_member = room_network.GetRoomMember().lock()) {
        proxy_packet_received = room_member->BindOnProxyPacketReceived(
            [this](const Network::ProxyPacket& packet) { OnProxyPacketReceived(packet); });
    } else {
        LOG_ERROR(Service, "Network isn't initialized");
    }
}

BSD::~BSD() {
    if (auto room_member = room_network.GetRoomMember().lock()) {
        room_member->Unbind(proxy_packet_received);
    }

    ClearParkedUdpSockets();
}

std::unique_lock<std::mutex> BSD::LockService() {
    // Do not lock socket IClient instances.
    return {};
}

BSDCFG::BSDCFG(Core::System& system_) : ServiceFramework{system_, "bsdcfg"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSDCFG::SetIfUp, "SetIfUp"},
        {1, &BSDCFG::SetIfUpWithEvent, "SetIfUpWithEvent"},
        {2, &BSDCFG::CancelIf, "CancelIf"},
        {3, &BSDCFG::SetIfDown, "SetIfDown"},
        {4, &BSDCFG::GetIfState, "GetIfState"},
        {5, &BSDCFG::DhcpRenew, "DhcpRenew"},
        {6, &BSDCFG::AddStaticArpEntry, "AddStaticArpEntry"},
        {7, &BSDCFG::RemoveArpEntry, "RemoveArpEntry"},
        {8, &BSDCFG::LookupArpEntry, "LookupArpEntry"},
        {9, &BSDCFG::LookupArpEntry2, "LookupArpEntry2"},
        {10, &BSDCFG::ClearArpEntries, "ClearArpEntries"},
        {11, &BSDCFG::ClearArpEntries2, "ClearArpEntries2"},
        {12, &BSDCFG::PrintArpEntries, "PrintArpEntries"},
        {13, &BSDCFG::Unknown13, "Unknown13"},
        {14, &BSDCFG::Unknown14, "Unknown14"},
        {15, &BSDCFG::Unknown15, "Unknown15"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BSDCFG::~BSDCFG() = default;

// BSDCFG Service Method Stubs
void BSDCFG::SetIfUp(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetIfUp");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::SetIfUpWithEvent(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetIfUpWithEvent");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::CancelIf(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called CancelIf");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::SetIfDown(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetIfDown");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::GetIfState(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetIfState");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::DhcpRenew(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called DhcpRenew");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::AddStaticArpEntry(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called AddStaticArpEntry");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::RemoveArpEntry(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called RemoveArpEntry");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::LookupArpEntry(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called LookupArpEntry");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::LookupArpEntry2(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called LookupArpEntry2");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::ClearArpEntries(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called ClearArpEntries");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::ClearArpEntries2(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called ClearArpEntries2");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::PrintArpEntries(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called PrintArpEntries");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::Unknown13(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown13 (Cmd13)");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::Unknown14(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown14 (Cmd14)");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::Unknown15(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown15 (Cmd15)");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::GetResourceStatistics(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetResourceStatistics");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::GetSocketStatistics(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetSocketStatistics");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::GetThreadCoreMask(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetThreadCoreMask");
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<u64>(0);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Ioctl(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Ioctl");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(ENOTTY));
}

void BSD::NifIoctl(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called NifIoctl");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(ENOTTY));
}

void BSD::Open(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Open");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EACCES));
}

// [Nextendo] BSD::RecvMMsg's real implementation is defined below SendMMsg, alongside the
// shared MMsg* wire-format helpers both need (see bsd.h for its declaration).

void BSD::RegisterResourceStatisticsName(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called RegisterResourceStatisticsName");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

namespace {
// [Nextendo] Wire format for a single nn::socket msghdr inside a SendMMsg/RecvMMsg buffer.
// Mirrors Ryujinx-Nextendo's BsdMsgHdr::Deserialize/Serialize byte-for-byte (itself matching
// the real NX ABI): msg_namelen, [name], iov_count, iov_count * (u64 len, [data]),
// control_len, [control], flags, then a trailing "length" field the real syscall uses to
// report bytes transferred for that specific message.
struct MMsgEntry {
    std::vector<u8> name;
    std::vector<std::vector<u8>> iov;
    std::vector<u8> control;
    u32 flags = 0;
    u32 length = 0;
};

// [Nextendo] Diagnostic only -- logs the TLS record type/length (and handshake message type,
// if applicable) of a SendMMsg/RecvMMsg payload, matching Ryujinx-Nextendo's own
// "[DIAG] Bsd.SendMMsg TLS hs=0x.. len=.." logging (ManagedSocket.cs), generalized to cover
// every record type (Ryujinx's only fires for content-type Handshake) so a final small
// ApplicationData or Alert record right before a connection close is visible too. The TLS
// record header (type, version, length) is always plaintext even once the session is
// encrypted -- only the payload itself is opaque -- so this costs nothing and reveals real
// protocol state without decrypting anything.
void LogTlsRecordDiag(const char* what, std::span<const u8> data) {
    if (data.size() < 5) {
        return;
    }
    const u8 content_type = data[0];
    if (content_type < 0x14 || content_type > 0x17) {
        return; // not a TLS record (or content_type is genuinely opaque post-handshake noise)
    }
    const u16 record_len = static_cast<u16>((data[3] << 8) | data[4]);
    const char* type_name = [content_type] {
        switch (content_type) {
        case 0x14:
            return "ChangeCipherSpec";
        case 0x15:
            return "Alert";
        case 0x16:
            return "Handshake";
        case 0x17:
            return "ApplicationData";
        default:
            return "?";
        }
    }();
    if (content_type == 0x16 && data.size() > 5) {
        LOG_INFO(Service, "[Nextendo][DIAG] {} TLS type={}(0x{:02x}) hs=0x{:02x} record_len={} buf_len={}",
                 what, type_name, content_type, data[5], record_len, data.size());
    } else {
        LOG_INFO(Service, "[Nextendo][DIAG] {} TLS type={}(0x{:02x}) record_len={} buf_len={}", what,
                 type_name, content_type, record_len, data.size());
    }
}

bool MMsgReadU32(std::span<const u8>& data, u32& out) {
    if (data.size() < sizeof(u32)) {
        return false;
    }
    std::memcpy(&out, data.data(), sizeof(u32));
    data = data.subspan(sizeof(u32));
    return true;
}

bool MMsgReadU64(std::span<const u8>& data, u64& out) {
    if (data.size() < sizeof(u64)) {
        return false;
    }
    std::memcpy(&out, data.data(), sizeof(u64));
    data = data.subspan(sizeof(u64));
    return true;
}

bool MMsgReadBytes(std::span<const u8>& data, size_t count, std::vector<u8>& out) {
    if (data.size() < count) {
        return false;
    }
    out.assign(data.begin(), data.begin() + count);
    data = data.subspan(count);
    return true;
}

// Mirrors BsdMMsgHdr::Deserialize -- false on a malformed/short buffer (EFAULT in the
// reference implementation).
bool MMsgDeserialize(std::span<const u8> data, s32 vlen, std::vector<MMsgEntry>& out) {
    if (data.empty() || vlen < 0) {
        return false;
    }
    data = data.subspan(1); // leading header byte -- ignored (real hardware ignores it too)

    out.resize(static_cast<size_t>(vlen));
    for (auto& msg : out) {
        u32 name_len;
        if (!MMsgReadU32(data, name_len)) {
            return false;
        }
        if (name_len > 0 && !MMsgReadBytes(data, name_len, msg.name)) {
            return false;
        }

        u32 iov_count;
        if (!MMsgReadU32(data, iov_count)) {
            return false;
        }
        msg.iov.resize(iov_count);
        for (auto& segment : msg.iov) {
            u64 iov_len;
            if (!MMsgReadU64(data, iov_len)) {
                return false;
            }
            if (iov_len > 0 && !MMsgReadBytes(data, static_cast<size_t>(iov_len), segment)) {
                return false;
            }
        }

        u32 control_len;
        if (!MMsgReadU32(data, control_len)) {
            return false;
        }
        if (control_len > 0 && !MMsgReadBytes(data, control_len, msg.control)) {
            return false;
        }

        if (!MMsgReadU32(data, msg.flags)) {
            return false;
        }
        u32 unused_input_length;
        if (!MMsgReadU32(data, unused_input_length)) {
            return false;
        }
    }
    return true;
}

// Mirrors BsdMMsgHdr::Serialize -- writes the same shape back with each entry's (now updated)
// length field, so the guest can see how many bytes actually went out per message.
void MMsgSerialize(std::vector<u8>& out, const std::vector<MMsgEntry>& messages) {
    out.push_back(0x8);
    for (const auto& msg : messages) {
        const auto append_u32 = [&](u32 value) {
            const auto* bytes = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(value));
        };
        const auto append_u64 = [&](u64 value) {
            const auto* bytes = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(value));
        };

        append_u32(static_cast<u32>(msg.name.size()));
        out.insert(out.end(), msg.name.begin(), msg.name.end());

        append_u32(static_cast<u32>(msg.iov.size()));
        for (const auto& segment : msg.iov) {
            append_u64(segment.size());
            out.insert(out.end(), segment.begin(), segment.end());
        }

        append_u32(static_cast<u32>(msg.control.size()));
        out.insert(out.end(), msg.control.begin(), msg.control.end());

        append_u32(msg.flags);
        append_u32(msg.length);
    }
}
} // Anonymous namespace

void BSD::SendMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 vlen = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} vlen={} flags=0x{:x}", fd, vlen, flags);

    // [Nextendo] Was a hard EOPNOTSUPP stub that never touched the socket at all -- every
    // title that uses sendmmsg() for its TLS/gRPC handshake data (Splatoon 3's
    // NintendoSDK_gRPC_For_NPLN stack does) had that data silently dropped: the underlying
    // TCP connection could complete perfectly (confirmed: SO_ERROR came back 0) and the game
    // would still sit forever waiting for a response to a handshake it never actually sent.
    // Sends each message individually via the same SendImpl every other BSD send call uses
    // (including its TLS SNI injection for a ClientHello) rather than Ryujinx-Nextendo's
    // single batched scatter-gather send -- functionally equivalent for both a stream socket
    // (message boundaries don't matter) and a datagram one (each message is its own
    // datagram), just simpler and lower-risk to get right.
    if (!IsFileDescriptorValid(fd)) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::BADF);
        return;
    }

    // [Nextendo] This buffer is a "receive" (B) descriptor: the game pre-populates it with the
    // message data to send and expects the service to both read that AND write the per-message
    // length fields back into the SAME buffer afterward (a real in-out MapAlias, not the usual
    // one-way A-in/B-out split -- matches Ryujinx-Nextendo reading and writing the exact same
    // ReceiveBuff[0] region). ctx.ReadBuffer() only ever looks at A/X descriptors and silently
    // returned empty for this call; read B's raw guest memory directly instead.
    std::vector<u8> raw_buffer;
    if (!ctx.BufferDescriptorB().empty() && ctx.BufferDescriptorB()[0].Size() > 0) {
        raw_buffer.resize(ctx.BufferDescriptorB()[0].Size());
        ctx.GetMemory().ReadBlock(ctx.BufferDescriptorB()[0].Address(), raw_buffer.data(),
                                   raw_buffer.size());
    } else {
        auto fallback = ctx.ReadBuffer(0);
        raw_buffer.assign(fallback.begin(), fallback.end());
    }

    std::vector<MMsgEntry> messages;
    if (!MMsgDeserialize(raw_buffer, vlen, messages)) {
        LOG_ERROR(Service, "SendMMsg fd={} vlen={}: malformed message buffer", fd, vlen);
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::INVAL);
        return;
    }

    s32 processed = 0;
    Errno last_errno = Errno::SUCCESS;
    for (auto& msg : messages) {
        std::vector<u8> concatenated;
        for (const auto& segment : msg.iov) {
            concatenated.insert(concatenated.end(), segment.begin(), segment.end());
        }

        LogTlsRecordDiag("SendMMsg", concatenated);
        if (Kernel::Svc::IsNextendoDeadlineWatchActive()) {
            const auto* cur = Kernel::GetCurrentThreadPointer(system.Kernel());
            LOG_INFO(Service, "[Nextendo][SCHED-WATCH] SendMMsg on thread id={} prio={}",
                     cur ? cur->GetThreadId() : 0, cur ? cur->GetPriority() : -1);
        }
        auto [sent, send_errno] = SendImpl(fd, flags | msg.flags, concatenated);
        if (send_errno != Errno::SUCCESS) {
            last_errno = send_errno;
            // A message already sent stays sent -- only stop processing further ones, same
            // as the real syscall returning a short count on a mid-batch failure.
            break;
        }
        msg.length = sent < 0 ? 0 : static_cast<u32>(sent);
        ++processed;
    }

    std::vector<u8> write_buffer;
    MMsgSerialize(write_buffer, messages);
    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(last_errno == Errno::SUCCESS ? processed : -1);
    rb.PushEnum(last_errno);
}

void BSD::RecvMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 vlen = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();
    rp.Pop<u32>(); // reserved, unused -- matches Ryujinx-Nextendo's own RecvMMsg signature
    struct TimeVal {
        u64 tv_sec;
        u64 tv_usec;
    };
    static_assert(sizeof(TimeVal) == 16);
    [[maybe_unused]] const auto timeout = rp.PopRaw<TimeVal>();

    LOG_DEBUG(Service, "called. fd={} vlen={} flags=0x{:x}", fd, vlen, flags);

    // [Nextendo] Same fix as SendMMsg above (was a hard EOPNOTSUPP stub) -- a title that
    // batches its sends via sendmmsg() needs the matching recvmmsg() to read the response, or
    // the handshake goes out fine and the reply can never come back (confirmed directly:
    // Splatoon 3 calls this immediately after SendMMsg on the same fd). `timeout` is
    // deliberately unused, matching Ryujinx-Nextendo's own reference implementation (see its
    // TODO on ManagedSocket.RecvMMsg) -- non-blocking behavior still comes the normal way,
    // through FLAG_MSG_DONTWAIT / O_NONBLOCK.
    if (!IsFileDescriptorValid(fd)) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::BADF);
        return;
    }

    std::vector<u8> raw_buffer;
    if (!ctx.BufferDescriptorB().empty() && ctx.BufferDescriptorB()[0].Size() > 0) {
        raw_buffer.resize(ctx.BufferDescriptorB()[0].Size());
        ctx.GetMemory().ReadBlock(ctx.BufferDescriptorB()[0].Address(), raw_buffer.data(),
                                   raw_buffer.size());
    } else {
        auto fallback = ctx.ReadBuffer(0);
        raw_buffer.assign(fallback.begin(), fallback.end());
    }

    std::vector<MMsgEntry> messages;
    if (!MMsgDeserialize(raw_buffer, vlen, messages)) {
        LOG_ERROR(Service, "RecvMMsg fd={} vlen={}: malformed message buffer", fd, vlen);
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::INVAL);
        return;
    }

    s32 processed = 0;
    Errno last_errno = Errno::SUCCESS;
    for (auto& msg : messages) {
        size_t capacity = 0;
        for (const auto& segment : msg.iov) {
            capacity += segment.size();
        }

        std::vector<u8> received(capacity);
        if (Kernel::Svc::IsNextendoDeadlineWatchActive()) {
            const auto* cur = Kernel::GetCurrentThreadPointer(system.Kernel());
            LOG_INFO(Service, "[Nextendo][SCHED-WATCH] RecvMMsg (pre-recv) on thread id={} prio={}",
                     cur ? cur->GetThreadId() : 0, cur ? cur->GetPriority() : -1);
        }
        auto [ret, recv_errno] = RecvImpl(fd, flags | msg.flags, received);
        if (recv_errno != Errno::SUCCESS) {
            LOG_INFO(Service, "RecvMMsg fd={} capacity={}: recv_errno={}", fd, capacity,
                     static_cast<int>(recv_errno));
            last_errno = recv_errno;
            break;
        }

        const size_t actual = ret < 0 ? 0 : static_cast<size_t>(ret);
        LogTlsRecordDiag("RecvMMsg", std::span<const u8>(received).first(actual));
        // Distribute the received bytes back across this message's iovs in the same order
        // they were laid out on the way in.
        size_t offset = 0;
        for (auto& segment : msg.iov) {
            if (offset < actual) {
                const size_t take = std::min(segment.size(), actual - offset);
                std::copy(received.begin() + offset, received.begin() + offset + take,
                          segment.begin());
            }
            offset += segment.size();
        }
        msg.length = static_cast<u32>(actual);
        ++processed;
    }

    std::vector<u8> write_buffer;
    MMsgSerialize(write_buffer, messages);
    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(last_errno == Errno::SUCCESS ? processed : -1);
    rb.PushEnum(last_errno);
}

void BSD::SetThreadCoreMask(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetThreadCoreMask [15.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::ShutdownAllSockets(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called ShutdownAllSockets");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::SocketExempt(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SocketExempt");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1); // fd
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown36(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown36 [18.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown37(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown37 [18.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown38(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown38 [18.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown39(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown39 [20.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown40(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown40 [20.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Sysctl(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Sysctl");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

} // namespace Service::Sockets
