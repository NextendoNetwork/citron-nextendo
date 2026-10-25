// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cassert>
#include <string>

#include "web_service/npln_patch_catalog.h"

int main(int argc, char**) {
    using namespace WebService::NplnPatchCatalog;
    constexpr std::uint64_t title = 0x0100000000000001ULL;
    const std::string build(64, 'A');
    StartTitleIndexCheck();
    StartTitleIndexCheck();
    // Launch immediately, while the delayed initial request is still in flight.
    WaitForTitleIndex();
    // Classics must remain discoverable with the older public title index or
    // on the first offline launch. Compatibility never supplies unsigned bytes.
    for (auto classics : {0x0100D870045B6000ULL, 0x01008D300C50C000ULL,
                          0x0100C62011050000ULL, 0x010012F017576000ULL}) {
        assert(IsCompatibleTitle(classics));
        assert(!HasEntry(classics, build, "main"));
        assert(!Find(classics, build, "main", {}).authoritative);
    }
    if (argc > 1) {
        assert(!IsCompatibleTitle(title));
        assert(IsCompatibleTitle(0x0100965017338000ULL));
        PrepareForGame(title, build);
        assert(!HasEntry(title, build, "main"));
        return 0;
    }
    assert(IsCompatibleTitle(title));
    assert(IsCompatibleTitle(
        0x0100965017338000ULL)); // Integrated compatibility survives a partial index.
    assert(!IsCompatibleTitle(0x0100000000000002ULL));
    PrepareForGame(title, build);
    assert(HasEntry(title, build, "main"));
    assert(!HasEntry(title, std::string(64, 'B'), "main"));
    std::array<std::uint8_t, 260> image{};
    image[256] = 1;
    const auto selected = Find(title, build, "main", image);
    assert(selected.authoritative && !selected.ips.empty());
    for (int i = 0; i < 100; ++i) {
        assert(HasEntry(title, build, "main"));
        assert(Find(title, build, "main", image).ips == selected.ips);
    }
    EndGame();
    PrepareForGame(title, build);
    assert(HasEntry(title, build, "main"));
    EndGame();
}
