// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include "hid_core/resources/touch_screen/applet_touch_state.h"

int main() {
    using namespace Service::HID;
    TouchScreenState click{};
    click.sampling_number = 100;
    click.entry_count = 1;
    click.states[0].finger = 7;
    click.states[0].position = {640, 360};
    TouchFingerMap foreground{}, background{};
    foreground.touch_mode = Core::HID::TouchScreenModeForNx::Finger;
    const auto active = MakeAppletTouchState(click, true, foreground);
    const auto hidden = MakeAppletTouchState(click, false, background);
    if (active.entry_count != 1 || active.sampling_number != 100 ||
        active.states[0].position.x != 640 || foreground.finger_ids[0] != 7) return 1;
    if (hidden.entry_count != 0 || hidden.sampling_number != 100 ||
        background.finger_count != 0 || hidden.states[0].position.x != 0) return 2;
    if (click.entry_count != 1 || click.states[0].position.x != 640) return 3;
    const auto unfocused = MakeAppletTouchState(click, false, foreground);
    if (unfocused.entry_count != 0 || foreground.finger_ids[0] != 0) return 4;
    click.entry_count = 0;
    ++click.sampling_number;
    const auto release = MakeAppletTouchState(click, true, foreground);
    if (release.entry_count != 0 || release.sampling_number != 101 ||
        release.states[0].position.x != 0) return 5;
    click.entry_count = 100;
    if (MakeAppletTouchState(click, true, foreground).entry_count != 16) return 6;
    click.entry_count = -1;
    if (MakeAppletTouchState(click, true, foreground).entry_count != 0) return 7;
    if (foreground.touch_mode != Core::HID::TouchScreenModeForNx::Finger) return 8;
    std::cout << "PASS: foreground click; background isolation; input unchanged; focus loss; release; count bounds; touch mode retained\n";
}
