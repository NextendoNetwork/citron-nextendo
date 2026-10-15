// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <limits>
#include <thread>
#include "core/hle/service/domain_object_table.h"

struct Object {
    int value{};
    std::function<void()> on_destroy;
    ~Object() { if (on_destroy) on_destroy(); }
};

int main() {
    Service::DomainObjectTable<Object> table;
    table.Initialize(std::make_shared<Object>());
    if (!table.Get(std::numeric_limits<std::size_t>::max()).expired()) return 1;
    if (table.Close(std::numeric_limits<std::size_t>::max())) return 2;
    constexpr std::size_t workers = 8, per_worker = 2048;
    std::vector<std::vector<std::size_t>> ids(workers);
    std::vector<std::thread> threads;
    std::atomic<bool> failed{};
    for (std::size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            for (std::size_t i = 0; i < per_worker; ++i) {
                auto object = std::make_shared<Object>();
                object->value = static_cast<int>(worker);
                const auto id = table.Append(object);
                ids[worker].push_back(id);
                auto active = table.Get(id - 1).lock();
                if (active != object || !table.Close(id - 1) || table.Close(id - 1)) failed = true;
                if (!table.Get(id - 1).expired() || active->value != static_cast<int>(worker)) failed = true;
                (void)table.Get((id / 2)).lock(); // Concurrent growth/lookup of other entries.
            }
        });
    }
    for (auto& thread : threads) thread.join();
    std::vector<std::size_t> all;
    for (auto& batch : ids) all.insert(all.end(), batch.begin(), batch.end());
    std::sort(all.begin(), all.end());
    for (std::size_t i = 0; i < all.size(); ++i) if (all[i] != i + 2) failed = true;
    if (failed) return 3;
    auto reentrant = std::make_shared<Object>();
    bool destroyed{};
    reentrant->on_destroy = [&] { (void)table.Size(); destroyed = true; };
    const auto id = table.Append(std::move(reentrant));
    if (!table.Close(id - 1) || !destroyed) return 4;
    std::cout << "PASS: 16384 concurrent append/get/close operations; unique IDs; invalid indexes; active reference lifetime; reentrant destruction\n";
}
