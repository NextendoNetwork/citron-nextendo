// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nzp_online_count.h"

#include <atomic>
#include <thread>

#include <QObject>
#include <QTimer>

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace Nextendo::NzpOnlineCount {

namespace {

std::atomic<int> g_count{0};

class Poller : public QObject {
public:
    using QObject::QObject;

    void Poll() {
#ifdef ENABLE_WEB_SERVICE
        std::thread{[] { g_count.store(WebService::NextendoApi::GetNzpOnlineCount()); }}.detach();
#endif
    }
};

Poller*& PollerInstance() {
    static Poller* instance = nullptr;
    return instance;
}

} // Anonymous namespace

void Start(QObject* parent) {
    Poller*& instance = PollerInstance();
    if (instance) {
        return;
    }

    instance = new Poller(parent);
    auto* timer = new QTimer(instance);
    timer->setInterval(5000);
    QObject::connect(timer, &QTimer::timeout, instance, &Poller::Poll);
    timer->start();
    instance->Poll();
}

int Get() {
    return g_count.load();
}

} // namespace Nextendo::NzpOnlineCount
