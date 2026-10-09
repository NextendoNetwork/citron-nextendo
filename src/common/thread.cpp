// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string>
#include <vector>

#include "common/error.h"
#include "common/logging.h"
#include "common/profiling.h"
#include "common/thread.h"
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#include "common/string_util.h"
#else
#if defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#else
#include <pthread.h>
#endif
#include <sched.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __FreeBSD__
#define cpu_set_t cpuset_t
#endif

namespace Common {

#ifdef _WIN32

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    auto handle = GetCurrentThread();
    int windows_priority = 0;
    switch (new_priority) {
    case ThreadPriority::Low:
        windows_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case ThreadPriority::Normal:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    case ThreadPriority::High:
        windows_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case ThreadPriority::VeryHigh:
        windows_priority = THREAD_PRIORITY_HIGHEST;
        break;
    case ThreadPriority::Critical:
        windows_priority = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    }
    SetThreadPriority(handle, windows_priority);
}

#else

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    pthread_t this_thread = pthread_self();

    const auto scheduling_type = SCHED_OTHER;
    s32 max_prio = sched_get_priority_max(scheduling_type);
    s32 min_prio = sched_get_priority_min(scheduling_type);
    u32 level = std::max(static_cast<u32>(new_priority) + 1, 4U);

    struct sched_param params;
    if (max_prio > min_prio) {
        params.sched_priority = min_prio + ((max_prio - min_prio) * level) / 4;
    } else {
        params.sched_priority = min_prio - ((min_prio - max_prio) * level) / 4;
    }

    pthread_setschedparam(this_thread, scheduling_type, &params);
}

#endif

#ifdef _MSC_VER

// Sets the debugger-visible name of the current thread.
void SetCurrentThreadName(const char* name) {
    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());
#if defined(CITRON_ENABLE_TRACY) && CITRON_ENABLE_TRACY
    tracy::SetThreadName(name);
#endif
}

bool SetCurrentThreadAffinityMask(u64 affinity_mask) {
    if (affinity_mask == 0) {
        return false;
    }
    const auto previous_mask = SetThreadAffinityMask(GetCurrentThread(), affinity_mask);
    return previous_mask != 0;
}

#else // !MSVC_VER, so must be POSIX threads

// MinGW with the POSIX threading model does not support pthread_setname_np
#if !defined(_WIN32) || defined(_MSC_VER)
void SetCurrentThreadName(const char* name) {
#ifdef __APPLE__
    pthread_setname_np(name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__)
    // Linux limits thread names to 15 characters and will outright reject any
    // attempt to set a longer name with ERANGE.
    std::string truncated(name, std::min(strlen(name), static_cast<size_t>(15)));
    if (int e = pthread_setname_np(pthread_self(), truncated.c_str())) {
        errno = e;
        LOG_ERROR(Common, "Failed to set thread name to '{}': {}", truncated, GetLastErrorMsg());
    }
#else
    pthread_setname_np(pthread_self(), name);
#endif
#if defined(CITRON_ENABLE_TRACY) && CITRON_ENABLE_TRACY
    tracy::SetThreadName(name);
#endif
}
#endif

#if defined(_WIN32)
void SetCurrentThreadName(const char* name) {
    // Do Nothing on MingW
#if defined(CITRON_ENABLE_TRACY) && CITRON_ENABLE_TRACY
    tracy::SetThreadName(name);
#endif
}
#endif

bool SetCurrentThreadAffinityMask(u64 affinity_mask) {
    if (affinity_mask == 0) {
        return false;
    }
#if defined(__linux__)
    cpu_set_t set{};
    CPU_ZERO(&set);
    for (u32 bit = 0; bit < 64; ++bit) {
        if ((affinity_mask >> bit) & 1ULL) {
            CPU_SET(static_cast<int>(bit), &set);
        }
    }
#if defined(__ANDROID__)
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
#else
    return false;
#endif
}

#endif

#if defined(_WIN32) && defined(_MSC_VER)

namespace {

struct DedicatedCorePlan {
    std::vector<ULONG> dedicated; // one CPU set (first logical processor) per slot
    std::vector<ULONG> others;    // every CPU set not on a dedicated core
};

DedicatedCorePlan BuildDedicatedCorePlan(std::size_t count) {
    DedicatedCorePlan plan;
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0) {
        return plan;
    }
    std::vector<u8> buffer(length);
    auto* const info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
    if (!GetSystemCpuSetInformation(info, length, &length, GetCurrentProcess(), 0)) {
        return plan;
    }
    struct Set {
        ULONG id;
        u8 core;
        u8 efficiency;
        u8 logical;
    };
    std::vector<Set> sets;
    u8 best_efficiency = 0;
    for (ULONG offset = 0; offset < length;) {
        const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (entry->Type == CpuSetInformation && entry->CpuSet.Group == 0) {
            sets.push_back({entry->CpuSet.Id, entry->CpuSet.CoreIndex,
                            entry->CpuSet.EfficiencyClass, entry->CpuSet.LogicalProcessorIndex});
            best_efficiency = std::max(best_efficiency, entry->CpuSet.EfficiencyClass);
        }
        offset += entry->Size;
    }
    // Physical performance cores, in order; hybrid efficiency cores are never dedicated.
    std::vector<u8> cores;
    for (const Set& set : sets) {
        if (set.efficiency == best_efficiency &&
            std::find(cores.begin(), cores.end(), set.core) == cores.end()) {
            cores.push_back(set.core);
        }
    }
    std::sort(cores.begin(), cores.end());
    // Leave core 0 (most OS interrupt work) and at least one more core for everything else.
    if (cores.size() < count + 2) {
        return plan;
    }
    const std::vector<u8> chosen(cores.begin() + 1, cores.begin() + 1 + count);
    for (const u8 core : chosen) {
        const Set* first = nullptr;
        for (const Set& set : sets) {
            if (set.core == core && (!first || set.logical < first->logical)) {
                first = &set;
            }
        }
        plan.dedicated.push_back(first->id);
    }
    for (const Set& set : sets) {
        if (std::find(chosen.begin(), chosen.end(), set.core) == chosen.end()) {
            plan.others.push_back(set.id);
        }
    }
    return plan;
}

// The process default only reaches threads created later; move the existing ones too.
void MoveExistingThreads(const std::vector<ULONG>& others) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) {
            continue;
        }
        const HANDLE thread = OpenThread(
            THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
            entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        ULONG selected = 0;
        GetThreadSelectedCpuSets(thread, nullptr, 0, &selected);
        if (selected == 0) {
            SetThreadSelectedCpuSets(thread, others.data(), static_cast<ULONG>(others.size()));
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

} // namespace

bool SetCurrentThreadDedicatedCore(std::size_t index, std::size_t count) {
    static const DedicatedCorePlan plan = [count] {
        DedicatedCorePlan built = BuildDedicatedCorePlan(count);
        if (!built.dedicated.empty() &&
            !SetProcessDefaultCpuSets(GetCurrentProcess(), built.others.data(),
                                      static_cast<ULONG>(built.others.size()))) {
            built.dedicated.clear();
        }
        if (!built.dedicated.empty()) {
            MoveExistingThreads(built.others);
        }
        if (built.dedicated.empty()) {
            LOG_INFO(Common, "Not dedicating host cores to emulated CPU threads");
        } else {
            LOG_INFO(Common, "Dedicating {} host performance cores to emulated CPU threads",
                     built.dedicated.size());
        }
        return built;
    }();
    if (index >= plan.dedicated.size()) {
        return false;
    }
    return SetThreadSelectedCpuSets(GetCurrentThread(), &plan.dedicated[index], 1) != FALSE;
}

#else

bool SetCurrentThreadDedicatedCore(std::size_t, std::size_t) {
    return false;
}

#endif

} // namespace Common
