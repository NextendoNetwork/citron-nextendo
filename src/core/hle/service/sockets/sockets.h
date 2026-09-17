// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_funcs.h"
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
}

namespace Service::Sockets {

enum class Errno : u32 {
    SUCCESS = 0,
    BADF = 9,
    AGAIN = 11,
    NOMEM = 12,
    INVAL = 22,
    MFILE = 24,
    PIPE = 32,
    MSGSIZE = 90,
    OPNOTSUPP = 95,
    AFNOSUPPORT = 97,
    CONNABORTED = 103,
    CONNRESET = 104,
    NOTCONN = 107,
    TIMEDOUT = 110,
    CONNREFUSED = 111,
    DESTADDRREQ = 89,
    INPROGRESS = 115,
};

enum class GetAddrInfoError : s32 {
    SUCCESS = 0,
    ADDRFAMILY = 1,
    AGAIN = 2,
    BADFLAGS = 3,
    FAIL = 4,
    FAMILY = 5,
    MEMORY = 6,
    NODATA = 7,
    NONAME = 8,
    SERVICE = 9,
    SOCKTYPE = 10,
    SYSTEM = 11,
    BADHINTS = 12,
    PROTOCOL = 13,
    OVERFLOW_ = 14, // avoid name collision with Windows macro
    OTHER = 15,
};

enum class Domain : u32 {
    Unspecified = 0,
    INET = 2,
    INET6 = 28,
};

enum class Type : u32 {
    Unspecified = 0,
    STREAM = 1,
    DGRAM = 2,
    RAW = 3,
    SEQPACKET = 5,
};

enum class Protocol : u32 {
    Unspecified = 0,
    ICMP = 1,
    TCP = 6,
    UDP = 17,
};

enum class SocketLevel : u32 {
    SOCKET = 0xffff, // i.e. SOL_SOCKET
};

enum class OptName : u32 {
    REUSEADDR = 0x4,
    KEEPALIVE = 0x8,
    BROADCAST = 0x20,
    LINGER = 0x80,
    SNDBUF = 0x1001,
    RCVBUF = 0x1002,
    SNDTIMEO = 0x1005,
    RCVTIMEO = 0x1006,
    ERROR_ = 0x1007,   // avoid name collision with Windows macro
    NOSIGPIPE = 0x800, // at least according to libnx
};

enum class ShutdownHow : s32 {
    RD = 0,
    WR = 1,
    RDWR = 2,
};

enum class FcntlCmd : s32 {
    GETFL = 3,
    SETFL = 4,
};

struct SockAddrIn {
    u8 len;
    u8 family;
    u16 portno;
    std::array<u8, 4> ip;
    std::array<u8, 8> zeroes;
};

enum class PollEvents : u16 {
    // Using Pascal case because IN is a macro on Windows.
    In = 1 << 0,
    Pri = 1 << 1,
    Out = 1 << 2,
    Err = 1 << 3,
    Hup = 1 << 4,
    Nval = 1 << 5,
    RdNorm = 1 << 6,
    RdBand = 1 << 7,
    WrBand = 1 << 8,
};

DECLARE_ENUM_FLAG_OPERATORS(PollEvents);

struct PollFD {
    s32 fd;
    PollEvents events;
    PollEvents revents;
};

struct Linger {
    u32 onoff;
    u32 linger;
};

void LoopProcess(Core::System& system);

// [Nextendo] BSD's own worker-thread pool is small and shared across every socket service
// registered in LoopProcess (bsd:a/bsd:s/bsd:u/bsdcfg/dns:priv/ethc:c/ethc:i/nsd:a/nsd:u/
// sfdnsres). A gRPC-based title (Splatoon 3) blocks one of those threads in Poll() waiting on
// its own eventfd, expecting a *different* guest thread's eventfd Write() to signal completion
// -- but if enough concurrent Polls are each blocking synchronously, that Write() IPC can't
// even get dispatched to a free thread, and the title hangs forever waiting on itself
// (Ryujinx-Nextendo hit and fixed the identical failure; see IClient.cs's deferred-poll
// comment). BSD::Poll uses this event (via HLERequestContext::SetIsDeferred) to give up its
// thread instead of blocking when nothing is ready yet and an eventfd is in the set; BSD's
// eventfd Write path signals it so the deferred poll gets re-checked. Set once from LoopProcess
// (ServerManager::ManageDeferral); left null, every Poll() just blocks as before -- nothing
// extra requires it to be set.
void SetBsdDeferralEvent(Kernel::KEvent* event);
Kernel::KEvent* GetBsdDeferralEvent();

} // namespace Service::Sockets
