// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "citron/setup_startup.h"

int main() {
    namespace fs = std::filesystem;
    using namespace Citron::SetupStartup;
    const auto root = fs::temp_directory_path() /
        ("citron-setup-startup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto shared = root / "Roaming/citron/config/qt-config.ini";
    assert(!ConfigurationExists(shared));
    assert(ShouldOffer(false, false, false, false, false));
    fs::create_directories(shared.parent_path());
    { std::ofstream file(shared); }
    assert(!ConfigurationExists(shared));
    // An older installation has real settings but no setupWizardSeen field.
    { std::ofstream file(shared); file << "[UI]\nfirstStart=false\n"; }
    assert(ConfigurationExists(shared));
    for (const auto& version : {"version-A", "version-B"}) {
        fs::create_directories(root / version);
        assert(!ShouldOffer(ConfigurationExists(shared), false, false, false, false));
    }
    for (unsigned flags = 1; flags < 16; ++flags) {
        assert(!ShouldOffer(false, flags & 1, flags & 2, flags & 4, flags & 8));
        assert(!ShouldOffer(true, flags & 1, flags & 2, flags & 4, flags & 8));
    }
    assert(!ConfigurationExists(shared.parent_path()));
    fs::remove(shared);
    fs::remove(shared.parent_path()); fs::remove(shared.parent_path().parent_path());
    fs::remove(root / "Roaming"); fs::remove(root / "version-A"); fs::remove(root / "version-B"); fs::remove(root);
}
