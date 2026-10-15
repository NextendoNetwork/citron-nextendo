// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/psc/ovln/ovln_types.h"

namespace Service::PSC {
// One bus per PSC service process. Receivers unregister before closing their event.
class NotificationBus {
public:
    struct Message {
        OverlayNotification data;
        u64 tick;
    };
    struct Receiver {
        Kernel::KEvent* event;
        std::map<std::string, std::deque<Message>> sources;
    };
    std::mutex mutex;
    std::vector<Receiver*> receivers;

    void Send(const std::string& source, const OverlayNotification& data, u64 tick) {
        std::scoped_lock lock{mutex};
        for (auto* receiver : receivers) {
            const auto it = receiver->sources.find(source);
            if (it == receiver->sources.end()) {
                continue;
            }
            auto& queue = it->second;
            // Bound the host queue if a guest stops consuming notifications.
            if (queue.size() == 64) {
                queue.pop_front();
            }
            queue.push_back({data, tick});
            receiver->event->Signal();
        }
    }
};
} // namespace Service::PSC
