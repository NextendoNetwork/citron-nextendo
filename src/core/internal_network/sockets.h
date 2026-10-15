// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#if defined(_WIN32)
#elif !(defined(__unix__) || defined(__APPLE__))
#error "Platform not implemented"
#endif

#include "common/common_types.h"
#include "core/internal_network/network.h"

// TODO: C++20 Replace std::vector usages with std::span

namespace Network {

struct ProxyPacket;

#ifdef _WIN32
using SOCKET = ::SOCKET;
#else
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
#endif

class SocketBase {
public:
    struct AcceptResult {
        std::unique_ptr<SocketBase> socket;
        SockAddrIn sockaddr_in;
    };

    SocketBase() = default;
    explicit SocketBase(SOCKET fd_) : fd{fd_} {}
    virtual ~SocketBase() = default;

    CITRON_NON_COPYABLE(SocketBase);
    CITRON_NON_MOVEABLE(SocketBase);

    virtual Errno Initialize(Domain domain, Type type, Protocol protocol) = 0;

    virtual Errno Close() = 0;

    virtual std::pair<AcceptResult, Errno> Accept() = 0;

    virtual Errno Connect(SockAddrIn addr_in) = 0;

    virtual std::pair<SockAddrIn, Errno> GetPeerName() = 0;

    virtual std::pair<SockAddrIn, Errno> GetSockName() = 0;

    virtual Errno Bind(SockAddrIn addr) = 0;

    virtual Errno Listen(s32 backlog) = 0;

    virtual Errno Shutdown(ShutdownHow how) = 0;

    virtual std::pair<s32, Errno> Recv(int flags, std::span<u8> message) = 0;

    virtual std::pair<s32, Errno> RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) = 0;

    virtual std::pair<s32, Errno> Send(std::span<const u8> message, int flags) = 0;

    virtual std::pair<s32, Errno> SendTo(u32 flags, std::span<const u8> message,
                                         const SockAddrIn* addr) = 0;

    virtual Errno SetLinger(bool enable, u32 linger) = 0;

    // [Nextendo] Splatoon 3's gRPC/NintendoSDK_NPLN stack sets SO_LINGER (via its vendor-private
    // optname alias) once right after every connect, matching the SetReuseAddr/GetReuseAddr
    // pattern below -- see bsd.cpp's SetSockOptImpl/GetSockOptImpl for the observed values.
    virtual std::tuple<bool, u32, Errno> GetLinger() = 0;

    virtual Errno SetReuseAddr(bool enable) = 0;

    virtual Errno SetKeepAlive(bool enable) = 0;

    virtual Errno SetBroadcast(bool enable) = 0;

    // [Nextendo] A portable networking stack (Splatoon 3's gRPC/NintendoSDK_NPLN stack does
    // this) commonly reads a SOL_SOCKET option straight back after setting it to confirm the
    // set actually took, before trusting the socket -- SetReuseAddr/SetKeepAlive/SetBroadcast
    // had no matching getters at all, so that verification always failed and the game abandoned
    // an otherwise-fine socket, retrying the whole connection sequence from scratch forever
    // (confirmed directly: TCP_NODELAY was the first casualty, SO_REUSEADDR the next -- see
    // GetNoDelay's declaration comment above for the TCP_NODELAY half of this).
    virtual std::pair<bool, Errno> GetReuseAddr() = 0;

    virtual std::pair<bool, Errno> GetKeepAlive() = 0;

    virtual std::pair<bool, Errno> GetBroadcast() = 0;

    virtual Errno SetSndBuf(u32 value) = 0;

    virtual Errno SetRcvBuf(u32 value) = 0;

    // Native sockets report the effective host buffer size. Backends without
    // a native socket retain the unsupported-option result until implemented.
    virtual std::pair<u32, Errno> GetSndBuf() { return {0, Errno::INVAL}; }
    virtual std::pair<u32, Errno> GetRcvBuf() { return {0, Errno::INVAL}; }

    virtual Errno SetSndTimeo(u32 value) = 0;

    virtual Errno SetRcvTimeo(u32 value) = 0;

    virtual Errno SetNonBlock(bool enable) = 0;

    // [Nextendo] IPPROTO_TCP-level option, not SOL_SOCKET -- the generic SetSockOpt/GetSockOpt
    // helpers below hardcode SOL_SOCKET and can't reach it. A portable networking stack
    // (Splatoon 3's gRPC/NintendoSDK_NPLN stack does this) commonly disables Nagle's algorithm
    // and then reads TCP_NODELAY back to confirm it actually took before trusting the socket;
    // without a real answer here it abandons an otherwise-fine socket and retries the whole
    // connection sequence from scratch forever.
    virtual Errno SetNoDelay(bool enable) = 0;
    virtual std::pair<bool, Errno> GetNoDelay() = 0;

    virtual std::pair<Errno, Errno> GetPendingError() = 0;

    virtual bool IsOpened() const = 0;

    virtual void HandleProxyPacket(const ProxyPacket& packet) = 0;

    [[nodiscard]] SOCKET GetFD() const {
        return fd;
    }

protected:
    SOCKET fd = INVALID_SOCKET;
};

class Socket : public SocketBase {
public:
    Socket() = default;
    explicit Socket(SOCKET fd_) : SocketBase{fd_} {}

    ~Socket() override;

    Socket(Socket&& rhs) noexcept;

    Errno Initialize(Domain domain, Type type, Protocol protocol) override;

    Errno Close() override;

    std::pair<AcceptResult, Errno> Accept() override;

    Errno Connect(SockAddrIn addr_in) override;

    std::pair<SockAddrIn, Errno> GetPeerName() override;

    std::pair<SockAddrIn, Errno> GetSockName() override;

    Errno Bind(SockAddrIn addr) override;

    Errno Listen(s32 backlog) override;

    Errno Shutdown(ShutdownHow how) override;

    std::pair<s32, Errno> Recv(int flags, std::span<u8> message) override;

    std::pair<s32, Errno> RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) override;

    std::pair<s32, Errno> Send(std::span<const u8> message, int flags) override;

    std::pair<s32, Errno> SendTo(u32 flags, std::span<const u8> message,
                                 const SockAddrIn* addr) override;

    Errno SetLinger(bool enable, u32 linger) override;

    std::tuple<bool, u32, Errno> GetLinger() override;

    Errno SetReuseAddr(bool enable) override;

    Errno SetKeepAlive(bool enable) override;

    Errno SetBroadcast(bool enable) override;

    std::pair<bool, Errno> GetReuseAddr() override;

    std::pair<bool, Errno> GetKeepAlive() override;

    std::pair<bool, Errno> GetBroadcast() override;

    Errno SetSndBuf(u32 value) override;

    Errno SetRcvBuf(u32 value) override;

    std::pair<u32, Errno> GetSndBuf() override;
    std::pair<u32, Errno> GetRcvBuf() override;

    Errno SetSndTimeo(u32 value) override;

    Errno SetRcvTimeo(u32 value) override;

    Errno SetNonBlock(bool enable) override;

    Errno SetNoDelay(bool enable) override;
    std::pair<bool, Errno> GetNoDelay() override;

    template <typename T>
    Errno SetSockOpt(SOCKET fd, int option, T value);

    std::pair<Errno, Errno> GetPendingError() override;

    template <typename T>
    std::pair<T, Errno> GetSockOpt(SOCKET fd, int option);

    bool IsOpened() const override;

    void HandleProxyPacket(const ProxyPacket& packet) override;

private:
    bool is_non_blocking = false;
    // [Nextendo] What SetNoDelay was last actually told, independent of whether a later
    // GetNoDelay's own getsockopt() call can be trusted -- see the definition comment on
    // GetNoDelay in network.cpp.
    std::optional<bool> no_delay_cache;
};

std::pair<s32, Errno> Poll(std::vector<PollFD>& poll_fds, s32 timeout);

} // namespace Network
