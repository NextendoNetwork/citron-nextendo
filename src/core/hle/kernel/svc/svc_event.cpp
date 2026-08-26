// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/scope_exit.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc/nextendo_deadline_watch.h"

namespace Kernel::Svc {

Result SignalEvent(Core::System& system, Handle event_handle) {
    LOG_DEBUG(Kernel_SVC, "called, event_handle=0x{:08X}", event_handle);

    // Get the current handle table.
    const KHandleTable& handle_table = GetCurrentProcess(system.Kernel()).GetHandleTable();

    // Fail-safe for system applets
    const auto program_id = GetCurrentProcess(system.Kernel()).GetProgramId();
    if ((program_id & 0xFFFFFFFFFFFFFF00ull) == 0x0100000000001000ull) {
        KScopedAutoObject event = handle_table.GetObject<KEvent>(event_handle);
        if (event.IsNotNull()) {
            event->Signal();
        } else {
            LOG_WARNING(Kernel_SVC, "SignalEvent best-effort unknown handle=0x{:08X} (ignored)",
                        event_handle);
        }
        R_SUCCEED();
    }

    // Get the event.
    KScopedAutoObject event = handle_table.GetObject<KEvent>(event_handle);
    R_UNLESS(event.IsNotNull(), ResultInvalidHandle);

    if (IsNextendoDeadlineWatchActive()) {
        // [Nextendo][DIAG] Log the READABLE event's own object address, not just this write
        // handle's number -- a WaitSynchronization caller holds a *different* handle number for
        // the same underlying event pair (the read half), so correlating by raw handle value
        // across CreateEvent/SignalEvent/WaitSynchronization's own diagnostics doesn't work. The
        // readable-event object address is exactly what WaitSynchronization's ONE-SHOT probe
        // diagnostic (svc_synchronization.cpp) also prints, so the two can be matched directly.
        LOG_INFO(Kernel_SVC,
                 "[Nextendo][DIAG] SignalEvent write_handle=0x{:08X} readable_event_obj={}",
                 event_handle, static_cast<const void*>(std::addressof(event->GetReadableEvent())));
    }

    R_RETURN(event->Signal());
}

Result ClearEvent(Core::System& system, Handle event_handle) {
    LOG_TRACE(Kernel_SVC, "called, event_handle=0x{:08X}", event_handle);

    // Get the current handle table.
    const auto& handle_table = GetCurrentProcess(system.Kernel()).GetHandleTable();

    // Try to clear the writable event.
    {
        KScopedAutoObject event = handle_table.GetObject<KEvent>(event_handle);
        if (event.IsNotNull()) {
            R_RETURN(event->Clear());
        }
    }

    // Try to clear the readable event.
    {
        KScopedAutoObject readable_event = handle_table.GetObject<KReadableEvent>(event_handle);
        if (readable_event.IsNotNull()) {
            R_RETURN(readable_event->Clear());
        }
    }

    R_THROW(ResultInvalidHandle);
}

Result CreateEvent(Core::System& system, Handle* out_write, Handle* out_read) {
    LOG_DEBUG(Kernel_SVC, "called");

    // Get the kernel reference and handle table.
    auto& kernel = system.Kernel();
    auto& handle_table = GetCurrentProcess(kernel).GetHandleTable();

    // Reserve a new event from the process resource limit
    KScopedResourceReservation event_reservation(GetCurrentProcessPointer(kernel),
                                                 LimitableResource::EventCountMax);
    R_UNLESS(event_reservation.Succeeded(), ResultLimitReached);

    // Create a new event.
    KEvent* event = KEvent::Create(kernel);
    R_UNLESS(event != nullptr, ResultOutOfResource);

    // Initialize the event.
    event->Initialize(GetCurrentProcessPointer(kernel));

    // Commit the thread reservation.
    event_reservation.Commit();

    // Ensure that we clean up the event (and its only references are handle table) on function end.
    SCOPE_EXIT {
        event->GetReadableEvent().Close();
        event->Close();
    };

    // Register the event.
    KEvent::Register(kernel, event);

    // Add the event to the handle table.
    R_TRY(handle_table.Add(out_write, event));

    // Ensure that we maintain a clean handle state on exit.
    ON_RESULT_FAILURE {
        handle_table.Remove(*out_write);
    };

    // Add the readable event to the handle table.
    R_RETURN(handle_table.Add(out_read, std::addressof(event->GetReadableEvent())));
}

Result SignalEvent64(Core::System& system, Handle event_handle) {
    R_RETURN(SignalEvent(system, event_handle));
}

Result ClearEvent64(Core::System& system, Handle event_handle) {
    R_RETURN(ClearEvent(system, event_handle));
}

Result CreateEvent64(Core::System& system, Handle* out_write_handle, Handle* out_read_handle) {
    R_RETURN(CreateEvent(system, out_write_handle, out_read_handle));
}

Result SignalEvent64From32(Core::System& system, Handle event_handle) {
    R_RETURN(SignalEvent(system, event_handle));
}

Result ClearEvent64From32(Core::System& system, Handle event_handle) {
    R_RETURN(ClearEvent(system, event_handle));
}

Result CreateEvent64From32(Core::System& system, Handle* out_write_handle,
                           Handle* out_read_handle) {
    R_RETURN(CreateEvent(system, out_write_handle, out_read_handle));
}

} // namespace Kernel::Svc
