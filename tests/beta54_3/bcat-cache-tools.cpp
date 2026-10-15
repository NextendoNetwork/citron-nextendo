// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include "citron/bcat_cache_tools.h"
namespace fs = std::filesystem;
using namespace Citron::BcatCacheTools;
void write(const fs::path& p, const char* text) { fs::create_directories(p.parent_path()); std::ofstream(p) << text; }
std::string read(const fs::path& p) { std::ifstream f(p); return std::string(std::istreambuf_iterator<char>(f), {}); }
int main() {
    static_assert(DownloadTitle(0x0100C2500FC20000ULL) == 0x0100C2500FC20000ULL);
    static_assert(DownloadTitle(0x01003bc0000a0000ULL) == 0x0100f8f0000a2000ULL);
    static_assert(DownloadTitle(0x01003c700009c800ULL) == 0x0100f8f0000a2000ULL);
    static_assert(DownloadTitle(0x01008F6008C5E000ULL) == 0x01008F6008C5E000ULL);
    const auto root = fs::temp_directory_path() /
        ("bcat-cache-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto target = root / "cache", stage = root / "stage", backup = root / "backup";
    write(target / "old", "existing cache");
    assert(!CommitStage(stage, target, backup));
    assert(read(target / "old") == "existing cache");
    fs::create_directories(stage);
    assert(!CommitStage(stage, target, backup));
    assert(read(target / "old") == "existing cache");
    write(stage / "new", "new schedule");
    write(backup / "occupied", "reserved");
    assert(!CommitStage(stage, target, backup));
    assert(read(target / "old") == "existing cache");
    assert(read(stage / "new") == "new schedule");
    const auto safe_backup = root / "previous-cache";
    assert(CommitStage(stage, target, safe_backup));
    assert(read(target / "new") == "new schedule");
    assert(!fs::exists(target / "old"));
    assert(read(safe_backup / "old") == "existing cache");
    write(root / "fresh-stage" / "new", "first install");
    assert(CommitStage(root / "fresh-stage", root / "fresh-cache", root / "fresh-backup"));
    assert(read(root / "fresh-cache" / "new") == "first install");
    std::cout << "PASS: game-specific title routing, missing/empty stages and backup conflict preserve cache; complete replacement retains backup; first install\n";
}
