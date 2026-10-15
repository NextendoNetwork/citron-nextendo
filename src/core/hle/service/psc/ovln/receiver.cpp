// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/kernel/k_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/psc/ovln/receiver.h"

// [UNITY-FIX] undef Win32 macros shadowing ServiceContext methods.
#undef CreateEvent
#undef CreateMutex
#undef CreateSemaphore


namespace Service::PSC {

IReceiver::IReceiver(Core::System& system_, std::shared_ptr<NotificationBus> bus_)
    : ServiceFramework{system_, "IReceiver"}, service_context{system_, "IReceiver"},
      bus{std::move(bus_)}, receiver{} {
    // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IReceiver::AddSource>, "AddSource"},
            {1, D<&IReceiver::RemoveSource>, "RemoveSource"},
            {2, D<&IReceiver::GetReceiveEventHandle>, "GetReceiveEventHandle"},
            {3, D<&IReceiver::Receive>, "Receive"},
            {4, D<&IReceiver::ReceiveWithTick>, "ReceiveWithTick"},
        };
    // clang-format on

    RegisterHandlers(functions);

    receive_event = service_context.CreateEvent("IReceiver::ReceiveEvent");
    receiver.event = receive_event;
    std::scoped_lock lock{bus->mutex};
    bus->receivers.push_back(&receiver);
}

IReceiver::~IReceiver() {
    std::scoped_lock lock{bus->mutex};
    std::erase(bus->receivers, &receiver);
    service_context.CloseEvent(receive_event);
}

Result IReceiver::AddSource(SourceName source_name) {
    std::scoped_lock lock{bus->mutex};
    const std::string name = source_name.GetString();
    LOG_INFO(Service_PSC, "called: source_name={}", name);

    // Add source if it doesn't already exist
    if (receiver.sources.find(name) == receiver.sources.end()) {
        receiver.sources[name] = {};
    }

    R_SUCCEED();
}

Result IReceiver::RemoveSource(SourceName source_name) {
    std::scoped_lock lock{bus->mutex};
    const std::string name = source_name.GetString();
    LOG_INFO(Service_PSC, "called: source_name={}", name);

    // Remove source if it exists
    receiver.sources.erase(name);
    if (std::ranges::all_of(receiver.sources, [](const auto& item) { return item.second.empty(); })) {
        receive_event->Clear();
    }

    R_SUCCEED();
}

Result IReceiver::GetReceiveEventHandle(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_INFO(Service_PSC, "called");
    *out_event = &receive_event->GetReadableEvent();
    R_SUCCEED();
}

Result IReceiver::Receive(Out<OverlayNotification> out_notification) {
    u64 tick;
    return ReceiveWithTick(out_notification, Out<u64>(&tick));
}

Result IReceiver::ReceiveWithTick(Out<OverlayNotification> out_notification, Out<u64> out_tick) {
    std::scoped_lock lock{bus->mutex};
    std::deque<NotificationBus::Message>* oldest = nullptr;
    for (auto& [name, queue] : receiver.sources) {
        if (!queue.empty() && (!oldest || queue.front().tick < oldest->front().tick)) {
            oldest = &queue;
        }
    }
    if (!oldest) {
        *out_notification = {};
        *out_tick = 0;
        receive_event->Clear();
        // The firmware receiver treats 25-12 as an empty queue.
        R_THROW(Result{0x1819});
    }
    *out_notification = oldest->front().data;
    *out_tick = oldest->front().tick;
    oldest->pop_front();
    if (std::ranges::all_of(receiver.sources, [](const auto& item) { return item.second.empty(); })) {
        receive_event->Clear();
    }
    R_SUCCEED();
}

} // namespace Service::PSC
