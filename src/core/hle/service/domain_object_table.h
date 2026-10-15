// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace Service {

// Cloned domain sessions share this table but can dispatch on different threads.
template <typename T>
class DomainObjectTable {
public:
    void Initialize(std::shared_ptr<T> root) {
        std::vector<std::shared_ptr<T>> previous;
        {
            std::scoped_lock lock{mutex};
            previous.swap(objects);
            objects.push_back(std::move(root));
        }
    }

    std::size_t Size() const {
        std::scoped_lock lock{mutex};
        return objects.size();
    }

    std::weak_ptr<T> Get(std::size_t index) const {
        std::scoped_lock lock{mutex};
        return index < objects.size() ? objects[index] : std::weak_ptr<T>{};
    }

    std::size_t Append(std::shared_ptr<T> object) {
        std::scoped_lock lock{mutex};
        objects.push_back(std::move(object));
        return objects.size(); // Native domain IDs are one-based.
    }

    bool Close(std::size_t index) {
        std::shared_ptr<T> closed;
        {
            std::scoped_lock lock{mutex};
            if (index >= objects.size() || !objects[index]) return false;
            closed = std::move(objects[index]);
        }
        // Service destructors can call back into IPC; release outside the table lock.
        return true;
    }

private:
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<T>> objects;
};

} // namespace Service
