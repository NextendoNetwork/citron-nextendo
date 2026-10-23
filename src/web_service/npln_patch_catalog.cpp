// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>

#include <fmt/format.h>
#include <httplib.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "web_service/npln_patch_catalog.h"
#include "web_service/npln_patch_manifest.h"
#include "web_service/npln_patch_state.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace WebService::NplnPatchCatalog {
namespace {

constexpr std::string_view PublicKey = "LAI+Pi+GZtj3p4DpNM5ORBraw1cbZCVSMFgUmAVWNeA=";
constexpr std::size_t MaxDocument = 1024 * 1024;
constexpr auto RefreshInterval = std::chrono::minutes(15);

struct State {
    std::mutex mutex;
    std::once_flag initialized;
    PatchState patches;
    TitleIndexState index;
    std::once_flag index_started;
    std::shared_future<void> index_check;
    std::chrono::steady_clock::time_point index_deadline;
    std::map<std::string, std::chrono::steady_clock::time_point> next_refresh;
    bool game_active{};
};

State& Cache() {
    static State state;
    return state;
}

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path CachePath() {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir) / "nextendo-npln-patches" /
           "catalog-v1.json";
}

std::string TargetName(std::uint64_t title_id, const std::string& build_id) {
    return fmt::format("{:016x}-{}", title_id, build_id);
}

void LoadVerifiedFile(const std::filesystem::path& path) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return;
        }

        std::string envelope(MaxDocument + 1, '\0');
        input.read(envelope.data(), static_cast<std::streamsize>(envelope.size()));
        envelope.resize(static_cast<std::size_t>(input.gcount()));
        const auto manifest = VerifyManifest(envelope, std::string(PublicKey), Now(), true);
        auto& state = Cache();
        std::scoped_lock lock(state.mutex);
        state.patches.Remember(manifest);
        LOG_INFO(WebService, "[Nextendo] NPLN patch cache verified, revision {}{}",
                 manifest.revision, manifest.expires <= Now() ? " (refresh due)" : "");
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "[Nextendo] NPLN patch cache rejected: {}", error.what());
    }
}

void LoadCache() {
    // Retain earlier builds even when a newer catalog no longer lists them.
    // Every archived envelope is verified again; unsigned extracted patches are never trusted.
    try {
        const auto directory = CachePath().parent_path() / "verified";
        if (std::filesystem::exists(directory)) {
            for (const auto& file : std::filesystem::directory_iterator(directory)) {
                if (file.is_regular_file() && file.path().extension() == ".json") {
                    LoadVerifiedFile(file.path());
                }
            }
        }
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "[Nextendo] NPLN patch archive unavailable: {}", error.what());
    }

    // Replay signed discovery history in revision order so partial lists preserve known titles.
    try {
        std::vector<TitleIndex> indexes;
        const auto directory = CachePath().parent_path() / "title-index";
        if (std::filesystem::exists(directory)) {
            for (const auto& file : std::filesystem::directory_iterator(directory)) {
                if (!file.is_regular_file() || file.path().extension() != ".json") {
                    continue;
                }
                try {
                    std::ifstream input(file.path(), std::ios::binary);
                    std::string body(MaxDocument + 1, '\0');
                    input.read(body.data(), static_cast<std::streamsize>(body.size()));
                    body.resize(static_cast<std::size_t>(input.gcount()));
                    indexes.push_back(VerifyTitleIndex(body, std::string(PublicKey), Now(), true));
                } catch (const std::exception& error) {
                    LOG_WARNING(WebService, "[Nextendo] NPLN title index cache rejected: {}",
                                error.what());
                }
            }
        }
        std::sort(indexes.begin(), indexes.end(),
                  [](const auto& a, const auto& b) { return a.revision < b.revision; });
        for (const auto& index : indexes) {
            Cache().index.Remember(index);
        }
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "[Nextendo] NPLN title index archive unavailable: {}",
                    error.what());
    }

    // Also import the cache written by the first catalog trial.
    LoadVerifiedFile(CachePath());
}

void SaveVerifiedFile(const std::filesystem::path& path, const std::string& envelope) {
    auto temporary = path;
    temporary += ".tmp";
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(envelope.data(), static_cast<std::streamsize>(envelope.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("Cannot persist verified patch catalog");
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace verified patch catalog cache");
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

void SaveCache(const std::string& envelope, std::uint64_t revision, const std::string& target) {
    const auto archive = CachePath().parent_path() / "verified" /
                         (target + "-revision-" + std::to_string(revision) + ".json");
    SaveVerifiedFile(archive, envelope);
    SaveVerifiedFile(CachePath().parent_path() / "patches" / (target + ".json"), envelope);
}

void DownloadTitleIndex() {
    try {
        auto& state = Cache();
        std::call_once(state.initialized, LoadCache);
        httplib::Client client("https://nextendo.network");
        client.set_connection_timeout(5);
        client.set_read_timeout(5);
        client.set_write_timeout(5);
        client.set_follow_location(false);
        client.enable_server_certificate_verification(true);
#ifdef __linux__
        for (const char* path : {"/etc/ssl/certs/ca-certificates.crt",
                                 "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"}) {
            if (std::filesystem::exists(path)) {
                client.set_ca_cert_path(path);
                break;
            }
        }
#endif
        std::string tag;
        {
            std::scoped_lock lock(state.mutex);
            tag = state.index.Tag();
        }
        httplib::Headers headers{{"User-Agent", "citron"},
                                 {"Accept", "application/json"},
                                 {"Cache-Control", "no-cache"}};
        if (!tag.empty()) {
            headers.emplace("If-None-Match", tag);
        }
        std::string body;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const auto response = client.Get("/service-catalog/v1/npln-title-index", headers,
                                         [&](const char* data, std::size_t length) {
                                             if (std::chrono::steady_clock::now() >= deadline ||
                                                 length > MaxDocument - body.size()) {
                                                 return false;
                                             }
                                             body.append(data, length);
                                             return true;
                                         });
        if (response && response->status == 304 && !tag.empty() && body.empty() &&
            response->get_header_value("ETag") == tag) {
            LOG_INFO(WebService,
                     "[Nextendo] NPLN title index unchanged; using verified local index");
            return;
        }
        if (!response || response->status != 200) {
            throw std::runtime_error("Title index download unavailable");
        }
        const auto index = VerifyTitleIndex(body, std::string(PublicKey), Now());
        if (response->get_header_value("ETag") != index.etag) {
            throw std::runtime_error("Title index response tag mismatch");
        }
        std::scoped_lock lock(state.mutex);
        auto next = state.index;
        next.Remember(index);
        SaveVerifiedFile(CachePath().parent_path() / "title-index" /
                             ("revision-" + std::to_string(index.revision) + ".json"),
                         body);
        state.index = std::move(next);
        LOG_INFO(WebService, "[Nextendo] NPLN title index verified, revision {}, {} titles",
                 index.revision, index.titles.size());
    } catch (const std::exception& error) {
        LOG_WARNING(WebService,
                    "[Nextendo] NPLN title index check failed; retaining verified index: {}",
                    error.what());
    }
}

void Download(std::uint64_t title_id, const std::string& build_id) {
    try {
        httplib::Client client("https://nextendo.network");
        client.set_connection_timeout(5);
        client.set_read_timeout(5);
        client.set_write_timeout(5);
        client.set_follow_location(false);
        client.enable_server_certificate_verification(true);
#ifdef __linux__
        for (const char* path : {"/etc/ssl/certs/ca-certificates.crt",
                                 "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"}) {
            if (std::filesystem::exists(path)) {
                client.set_ca_cert_path(path);
                break;
            }
        }
#endif

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::string body;
        auto& state = Cache();
        std::string tag;
        {
            std::scoped_lock lock(state.mutex);
            tag = state.patches.Tag(title_id, build_id);
        }
        httplib::Headers headers{{"User-Agent", "citron"}, {"Accept", "application/json"}};
        if (!tag.empty()) {
            headers.emplace("If-None-Match", tag);
        }
        const auto endpoint = fmt::format("/service-catalog/v1/npln-patches/{:016x}/{}",
                                          title_id, build_id);
        const auto response =
            client.Get(endpoint, headers,
                       [&](const char* data, std::size_t length) {
                           if (std::chrono::steady_clock::now() >= deadline ||
                               length > MaxDocument - body.size()) {
                               return false;
                           }
                           body.append(data, length);
                           return true;
                       });

        if (response && response->status == 304 && !tag.empty() &&
            response->get_header_value("ETag") == tag) {
            LOG_INFO(WebService, "[Nextendo] NPLN patch unchanged; using verified local copy ({})",
                     TargetName(title_id, build_id));
            return;
        }
        if (!response || response->status != 200) {
            throw std::runtime_error("Patch catalog download unavailable");
        }

        const auto manifest = VerifyManifest(body, std::string(PublicKey), Now());
        if (manifest.patches.size() != 1 || manifest.patches[0].title_id != title_id ||
            manifest.patches[0].build_id != build_id || manifest.patches[0].module != "main" ||
            manifest.etag.empty() || response->get_header_value("ETag") != manifest.etag) {
            throw std::runtime_error("Individual patch response does not match requested build");
        }
        std::scoped_lock lock(state.mutex);
        if (manifest.revision < state.patches.Revision(title_id, build_id)) {
            throw std::runtime_error("Patch catalog revision rollback rejected");
        }

        try {
            SaveCache(body, manifest.revision, TargetName(title_id, build_id));
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "[Nextendo] NPLN patch cache write failed: {}", error.what());
        }
        state.patches.Remember(manifest);
        LOG_INFO(WebService,
                 "[Nextendo] NPLN patch {} v{} downloaded and verified for build {}",
                 manifest.patches[0].id, manifest.patches[0].version, build_id);
    } catch (const std::exception& error) {
        LOG_WARNING(WebService,
                    "[Nextendo] NPLN patch catalog refresh failed; retaining verified patches: {}",
                    error.what());
    }
}

const Patch* Select(const State& state, std::uint64_t title_id, std::string_view build_id,
                    std::string_view module) {
    return state.patches.Find(title_id, build_id, module);
}

} // namespace

void StartTitleIndexCheck() {
    auto& state = Cache();
    std::call_once(state.index_started, [&state] {
        state.index_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        state.index_check = std::async(std::launch::async, DownloadTitleIndex).share();
    });
}

void WaitForTitleIndex() {
    StartTitleIndexCheck();
    auto& state = Cache();
    state.index_check.wait_until(state.index_deadline);
}

bool IsCompatibleTitle(std::uint64_t title_id) {
    auto& state = Cache();
    std::call_once(state.initialized, LoadCache);
    std::scoped_lock lock(state.mutex);

    constexpr std::array<std::uint64_t, 17> supported{{
        0x0100C2500FC20000ULL, // Splatoon 3
        0x010015100B514000ULL, // Super Mario Bros. Wonder
        0x0100C9A00ECE6000ULL, // Nintendo 64 - Nintendo Classics
        0x0100D870045B6000ULL, // NES 9.1.0
        0x01008D300C50C000ULL, // SNES 6.0.0
        0x0100C62011050000ULL, // Game Boy 4.1.0
        0x010012F017576000ULL, // Game Boy Advance 3.3.0
        0x0100C6F01C4F8000ULL, // METAL GEAR SOLID: Peace Walker
        0x0100F9F00C696000ULL, // Crash Team Racing Nitro-Fueled
        0x01006FD0080B2000ULL, // Overcooked! 2
        0x0100A3D008C5C000ULL, // Pokémon Scarlet
        0x01008F6008C5E000ULL, // Pokémon Violet
        0x0100F43008C44000ULL, // Pokémon Legends: Z-A
        0x0100453019AA8000ULL, // Xenoblade Chronicles X: Definitive Edition
        0x0100965017338000ULL, // Super Mario Party Jamboree
        0x0100B04011742000ULL, // Monster Hunter Rise
        0x0100559011740000ULL, // Monster Hunter Rise (Japan/Hong Kong)
    }};
    return state.index.HasTitle(title_id) || state.patches.HasTitle(title_id) ||
           std::find(supported.begin(), supported.end(), title_id) != supported.end();
}

void PrepareForGame(std::uint64_t title_id, const std::string& build_id) {
    WaitForTitleIndex();
    if (!IsCompatibleTitle(title_id)) {
        return;
    }
    if (build_id.size() != 64 || build_id.find_first_not_of("0123456789ABCDEF") != std::string::npos) {
        LOG_WARNING(WebService, "[Nextendo] Cannot check patch: invalid main build ID");
        return;
    }
    auto& state = Cache();
    std::call_once(state.initialized, LoadCache);

    bool refresh = false;
    const auto target = TargetName(title_id, build_id);
    {
        std::scoped_lock lock(state.mutex);
        if (state.game_active) {
            return;
        }

        const auto cached = state.patches.Cached(title_id, build_id);
        if (cached && cached->final) {
            state.patches.BeginGame(title_id);
            state.game_active = true;
            LOG_INFO(WebService, "[Nextendo] NPLN patch {} is final; using verified local copy",
                     cached->id);
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!state.next_refresh.contains(target)) {
            std::ifstream last_check(CachePath().parent_path() / "checks" / (target + ".txt"));
            std::int64_t checked_at{};
            const auto wall_now = Now();
            const auto interval =
                std::chrono::duration_cast<std::chrono::seconds>(RefreshInterval).count();
            if (last_check >> checked_at && checked_at > 0 && checked_at <= wall_now &&
                wall_now - checked_at < interval) {
                state.next_refresh[target] =
                    now + std::chrono::seconds(interval - (wall_now - checked_at));
            }
        }
        if (now >= state.next_refresh[target]) {
            state.next_refresh[target] = now + RefreshInterval;
            refresh = true;
        }
    }

    if (refresh) {
        try {
            const auto directory = CachePath().parent_path() / "checks";
            std::filesystem::create_directories(directory);
            std::ofstream last_check(directory / (target + ".txt"), std::ios::trunc);
            last_check << Now() << '\n';
            last_check.flush();
            if (!last_check) {
                throw std::runtime_error("Cannot persist catalog check time");
            }
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "[Nextendo] NPLN catalog check time not saved: {}",
                        error.what());
        }
        Download(title_id, build_id);
    }

    std::scoped_lock lock(state.mutex);
    state.patches.BeginGame(title_id);
    state.game_active = true;
    LOG_INFO(WebService, "[Nextendo] NPLN patches fixed for this game launch, catalog revision {}",
             state.patches.LatestRevision());
}

void EndGame() {
    auto& state = Cache();
    std::scoped_lock lock(state.mutex);
    state.patches.EndGame();
    state.game_active = false;
}

bool HasEntry(std::uint64_t title_id, std::string_view build_id, std::string_view module) {
    auto& state = Cache();
    std::scoped_lock lock(state.mutex);
    return Select(state, title_id, build_id, module) != nullptr;
}

Lookup Find(std::uint64_t title_id, std::string_view build_id, std::string_view module,
            std::span<const std::uint8_t> image) {
    auto& state = Cache();
    std::scoped_lock lock(state.mutex);
    const auto patch = Select(state, title_id, build_id, module);
    if (!patch) {
        return {};
    }

    Lookup result{true, patch->id, patch->version, {}};
    if (!patch->enabled) {
        LOG_WARNING(WebService, "[Nextendo] NPLN patch {} withdrawn by signed catalog", patch->id);
        return result;
    }
    if (!CheckGuards(*patch, image)) {
        LOG_ERROR(WebService,
                  "[Nextendo] NPLN patch {} rejected: original instructions do not match",
                  patch->id);
        return result;
    }

    result.ips = patch->ips;
    return result;
}

} // namespace WebService::NplnPatchCatalog
