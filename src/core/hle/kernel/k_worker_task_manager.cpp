// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_worker_task.h"
#include "core/hle/kernel/k_worker_task_manager.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

KWorkerTask::KWorkerTask(KernelCore& kernel) : KSynchronizationObject{kernel} {}

void KWorkerTask::DoWorkerTask() {
    if (auto* const thread = this->DynamicCast<KThread*>(); thread != nullptr) {
        return thread->DoWorkerTaskImpl();
    } else {
        auto* const process = this->DynamicCast<KProcess*>();
        ASSERT(process != nullptr);

        return process->DoWorkerTaskImpl();
    }
}

KWorkerTaskManager::KWorkerTaskManager()
    : m_waiting_thread(1, "KWorkerTaskManager"),
      m_process_waiting_thread(1, "KProcessExitWorker") {}

void KWorkerTaskManager::WaitForRequests() {
    // Keep the thread worker running while process tasks wait for child threads.
    // Process tasks may enqueue thread work, so drain that queue last.
    m_process_waiting_thread.WaitForRequests();
    m_waiting_thread.WaitForRequests();
}

void KWorkerTaskManager::AddTask(KernelCore& kernel, WorkerType type, KWorkerTask* task) {
    ASSERT(type <= WorkerType::Count);
    kernel.WorkerTaskManager().AddTask(kernel, task);
}

void KWorkerTaskManager::AddTask(KernelCore& kernel, KWorkerTask* task) {
    KScopedSchedulerLock sl(kernel);
    // KProcess::DoWorkerTaskImpl waits for KThread::FinishTermination to signal.
    // A single shared queue deadlocks if that thread task is queued behind the process.
    auto& worker = task->DynamicCast<KProcess*>() ? m_process_waiting_thread : m_waiting_thread;
    worker.QueueWork([task]() {
        // Do the task.
        task->DoWorkerTask();
    });
}

} // namespace Kernel
