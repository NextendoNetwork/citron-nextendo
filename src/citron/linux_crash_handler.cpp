// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/linux_crash_handler.h"

#ifdef __linux__

#include <cstdio>
#include <cstring>

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"

namespace LinuxCrashHandler {

namespace {

// Opened up front: the handler cannot allocate or take locks, so the fd and the alternate
// stack both have to exist before anything faults.
int g_report_fd = -1;
char g_alt_stack[64 * 1024]; // SIGSTKSZ is not a constant on modern glibc
struct sigaction g_old_segv {};
struct sigaction g_old_bus {};
struct sigaction g_old_ill {};
struct sigaction g_old_fpe {};
struct sigaction g_old_abrt {};

void WriteAll(const char* text) {
    const size_t len = std::strlen(text);
    void(write(STDERR_FILENO, text, len));
    if (g_report_fd >= 0) {
        void(write(g_report_fd, text, len));
    }
}

void WriteHex(unsigned long long value) {
    char buf[19] = "0x0000000000000000";
    for (int i = 0; i < 16; i++) {
        buf[17 - i] = "0123456789abcdef"[(value >> (i * 4)) & 0xf];
    }
    buf[18] = '\0';
    WriteAll(buf);
}

const char* SignalName(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "signal";
    }
}

struct sigaction* OldAction(int sig) {
    switch (sig) {
    case SIGSEGV:
        return &g_old_segv;
    case SIGBUS:
        return &g_old_bus;
    case SIGILL:
        return &g_old_ill;
    case SIGFPE:
        return &g_old_fpe;
    case SIGABRT:
        return &g_old_abrt;
    default:
        return nullptr;
    }
}

[[noreturn]] void Handler(int sig, siginfo_t* info, void* raw_context) {
    WriteAll("\n=== citron crashed: ");
    WriteAll(SignalName(sig));
    WriteAll(" ===\nfault address: ");
    WriteHex(reinterpret_cast<unsigned long long>(info ? info->si_addr : nullptr));

#if defined(__x86_64__)
    if (raw_context != nullptr) {
        const auto* uc = static_cast<const ucontext_t*>(raw_context);
        WriteAll("\nrip: ");
        WriteHex(static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RIP]));
    }
#elif defined(__aarch64__)
    if (raw_context != nullptr) {
        const auto* uc = static_cast<const ucontext_t*>(raw_context);
        WriteAll("\npc: ");
        WriteHex(static_cast<unsigned long long>(uc->uc_mcontext.pc));
    }
#endif

    WriteAll("\nbacktrace:\n");
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    if (g_report_fd >= 0) {
        backtrace_symbols_fd(frames, count, g_report_fd);
        void(fsync(g_report_fd));
    }
    WriteAll("=== end ===\n");

    // Hand back to whatever was installed before us so existing behaviour is unchanged,
    // then fall through to the default action.
    if (struct sigaction* old = OldAction(sig); old != nullptr) {
        if ((old->sa_flags & SA_SIGINFO) != 0 && old->sa_sigaction != nullptr) {
            old->sa_sigaction(sig, info, raw_context);
        } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN &&
                   old->sa_handler != nullptr) {
            old->sa_handler(sig);
        }
    }

    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    raise(sig);
    _exit(1);
}

} // Anonymous namespace

void Install() {
    const auto report_path = Common::FS::GetCitronPath(Common::FS::CitronPath::LogDir) / "crash_backtrace.txt";
    void(Common::FS::CreateParentDirs(report_path));
    g_report_fd = open(report_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    stack_t alt_stack{};
    alt_stack.ss_sp = g_alt_stack;
    alt_stack.ss_size = sizeof(g_alt_stack);
    sigaltstack(&alt_stack, nullptr);

    // Resolve the symbol table once here -- the first backtrace() call does a lazy dlopen of
    // libgcc, which is not something to attempt from inside a fault handler.
    void* warmup[4];
    void(backtrace(warmup, 4));

    struct sigaction sa {};
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
    sigaction(SIGILL, &sa, &g_old_ill);
    sigaction(SIGFPE, &sa, &g_old_fpe);
    sigaction(SIGABRT, &sa, &g_old_abrt);
}

} // namespace LinuxCrashHandler

#else

namespace LinuxCrashHandler {
void Install() {}
} // namespace LinuxCrashHandler

#endif
