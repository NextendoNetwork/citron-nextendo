// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <string>
#include "common/nextendo_viewer_presence.h"

using namespace Common::NextendoFriends;
std::string Description(const std::array<u8, 192>& field) {
    for (size_t p = 0; p < field.size() && field[p];) {
        std::string key(reinterpret_cast<const char*>(field.data() + p));
        p += key.size() + 1;
        assert(p < field.size());
        std::string value(reinterpret_cast<const char*>(field.data() + p));
        p += value.size() + 1;
        if (key == "sys_description") return value;
    }
    return {};
}
int main() {
    Entry e;
    e.status = 2; e.app_id = 1; e.app_name = "Nintendo 64"; e.platform = "citron";
    assert(Description(MakeViewerAppField(e)) == "Nintendo 64 · Citron");
    e.app_field = std::string("session\0opaque\0", 15);
    const auto original = e.app_field;
    auto field = MakeViewerAppField(e);
    assert(std::equal(original.begin(), original.end(), field.begin()));
    assert(Description(field) == "Nintendo 64 · Citron");
    assert(e.app_field == original);
    e.status = 0;
    assert(Description(MakeViewerAppField(e)).empty());
    e.status = 2;
    e.app_field = std::string("sys_description\0Game session\0", 29);
    assert(Description(MakeViewerAppField(e)) == "Game session");
    e.app_field.assign(192, 'x');
    field = MakeViewerAppField(e);
    assert(std::all_of(field.begin(), field.end(), [](u8 c) { return c == 'x'; }));
    e.app_field.clear(); e.platform = "unknown";
    assert(Description(MakeViewerAppField(e)) == "Nintendo 64");
    assert(ViewerApplicationId(e) == e.app_id);
    e.app_id = 0; e.app_detail = "HOME"; e.platform = "citron";
    assert(ViewerApplicationId(e) == Common::HomeMenuActivity::ApplicationId);
    assert(e.app_id == 0); // Viewer-only fallback must not change game-facing presence.
    e.status = 0;
    assert(ViewerApplicationId(e) == 0);
    e.status = 2; e.platform = "switch";
    assert(ViewerApplicationId(e) == 0);
    e.app_id = 0; e.app_detail = "HOME"; e.platform = "ryujinx";
    assert(Description(MakeViewerAppField(e)) == "qLaunch (Switch Menu) · Ryujinx");
    e.app_id = Common::HomeMenuActivity::ApplicationId;
    e.app_name = "Menu HOME"; e.app_detail.clear();
    assert(Description(MakeViewerAppField(e)) == "qLaunch (Switch Menu) · Ryujinx");
    e.platform = "citron";
    assert(Description(MakeViewerAppField(e)) == "qLaunch (Switch Menu) · Citron");
    e.app_id = 0; e.platform = "emulator";
    assert(ViewerApplicationId(e) == 0); // Generic idle presence does not prove HOME or Ryujinx.
    assert(Description(MakeViewerAppField(e)) == "Emulator");
    e.app_detail.clear(); e.platform.clear();
    assert(Description(MakeViewerAppField(e)).empty());
    e.app_id = 1; e.app_name = std::string(174, 'a') + "é";
    field = MakeViewerAppField(e);
    assert(Description(field) == std::string(174, 'a'));
    assert(field.back() == 0);
}
