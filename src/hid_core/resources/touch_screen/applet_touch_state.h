// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include "hid_core/resources/touch_screen/touch_types.h"

namespace Service::HID {

inline TouchScreenState MakeAppletTouchState(const TouchScreenState& input, bool enabled,
                                            TouchFingerMap& fingers) {
    TouchScreenState output{};
    output.sampling_number = input.sampling_number;
    output.entry_count = enabled ? std::clamp(input.entry_count, 0, static_cast<s32>(MaxFingers)) : 0;
    fingers.finger_count = output.entry_count;
    fingers.finger_ids = {};
    for (std::size_t i = 0; i < static_cast<std::size_t>(output.entry_count); ++i) {
        output.states[i] = input.states[i];
        fingers.finger_ids[i] = input.states[i].finger;
    }
    return output;
}

} // namespace Service::HID
