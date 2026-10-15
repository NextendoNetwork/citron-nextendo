// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <memory>
#include "common/slot_vector.h"
#include "video_core/buffer_cache/device_address_page_table.h"

int main() {
    using Table = VideoCommon::DeviceAddressPageTable<Common::SlotId, 16>;
    static_assert(std::tuple_size_v<Table> == (1ULL << (Tegra::MaxwellDeviceMemoryManager::AS_BITS - 16)));
    auto table = std::make_unique<Table>();
    constexpr std::size_t observed = 0x400001000ULL >> 16;
    constexpr std::size_t previous_limit = (1ULL << 34) >> 16;
    static_assert(observed >= previous_limit);
    if (observed >= table->size()) return 1;
    for (const auto index : {previous_limit - 1, observed, table->size() - 1}) {
        if ((*table)[index]) return 2;
        (*table)[index] = Common::SlotId{123};
        if ((*table)[index].index != 123) return 3;
        (*table)[index] = Common::SlotId{};
        if ((*table)[index]) return 4;
    }
    std::cout << "PASS: actual cache table covers observed fault address, old 16 GiB boundary and last 39-bit device page; default IDs remain invalid\n";
}
