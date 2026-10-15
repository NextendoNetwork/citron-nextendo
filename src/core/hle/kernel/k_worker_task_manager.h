// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/thread_worker.h"

namespace Kernel {

class KernelCore;
class KWorkerTask;

class KWorkerTaskManager final {
public:
    enum class WorkerType : u32 {
        Exit,
        Count,
    };

    KWorkerTaskManager();

    // The manager survives guest reboots; finish old tasks before kernel objects
    // and their slab storage are destroyed or reused by the next session.
    void WaitForRequests();

    static void AddTask(KernelCore& kernel, WorkerType type, KWorkerTask* task);

private:
    void AddTask(KernelCore& kernel, KWorkerTask* task);

private:
    Common::ThreadWorker m_waiting_thread;
    // Process termination waits for thread termination, so it cannot share its queue.
    Common::ThreadWorker m_process_waiting_thread;
};

} // namespace Kernel
