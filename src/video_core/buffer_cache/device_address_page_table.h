// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include "video_core/host1x/gpu_device_memory_manager.h"

namespace VideoCommon {

// Cache all device virtual addresses, not only physical DRAM-sized addresses.
template <typename Id, unsigned PageBits>
using DeviceAddressPageTable =
    std::array<Id, ((1ULL << Tegra::MaxwellDeviceMemoryManager::AS_BITS) >> PageBits)>;

} // namespace VideoCommon
