// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "web_service/ssbu_mod_installer.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"

#include "web_service/nextendo_api.h"

namespace WebService::SkylineMods {

namespace {

void ApplyCaCertPath(httplib::Client& client) {
    // Android has no CA path OpenSSL can read; the app exports one at startup.
    const std::string override_path = NextendoApi::GetCaCertPathOverride();
    if (!override_path.empty()) {
        client.set_ca_cert_path(override_path);
        return;
    }
#ifdef __linux__
    static constexpr std::array<const char*, 4> candidates{
        "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Arch
        "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL/CentOS
        "/etc/ssl/cert.pem",                  // Alpine
        "/etc/ssl/ca-bundle.pem",             // openSUSE
    };
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) {
            client.set_ca_cert_path(path);
            return;
        }
    }
#endif
}

bool MatchArcropolis(const std::string& name) {
    return name == "release.zip";
}

bool MatchNroHook(const std::string& name) {
    return name == "libnro_hook.nro";
}

bool MatchSmashline(const std::string& name) {
    return name == "libsmashline_plugin.nro";
}

bool MatchImguiSmash(const std::string& name) {
    // Not libimgui_smash.a, which is for plugin devs, not Skyline.
    return name == "libimgui_smash.nro";
}

bool MatchPiaInterface(const std::string& name) {
    return name == "libssbu_pia_manager.nro";
}

bool MatchOnlineDeluxe(const std::string& name) {
    // Filename is version-tagged, e.g. ssbu-online-deluxe-v1.3.0.zip.
    return name.starts_with("ssbu-online-deluxe") && name.ends_with(".zip");
}

struct ModRepoSpec {
    const char* display_name;
    const char* owner;
    const char* repo;
    bool (*matches)(const std::string&);
};

constexpr std::array<ModRepoSpec, 6> kSkylineMods{{
    {"arcropolis", "raytwo", "arcropolis", &MatchArcropolis},
    {"nro-hook-plugin", "ultimate-research", "nro-hook-plugin", &MatchNroHook},
    {"smashline", "HDR-Development", "smashline", &MatchSmashline},
    {"imgui-smash", "Coolsonickirby", "imgui-smash", &MatchImguiSmash},
    {"ssbu-pia-interface", "project-ultelier", "ssbu-pia-interface", &MatchPiaInterface},
    {"ssbu-online-deluxe", "saad-script", "ssbu-online-deluxe", &MatchOnlineDeluxe},
}};

// "https://host/a/b" -> {"https://host", "/a/b"}
std::pair<std::string, std::string> SplitUrl(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return {"", url};
    }
    const auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return {url, "/"};
    }
    return {url.substr(0, path_start), url.substr(path_start)};
}

ModFetchResult FetchOne(const ModRepoSpec& spec) {
    ModFetchResult result{};
    result.display_name = spec.display_name;
    result.release_page_url =
        std::string{"https://github.com/"} + spec.owner + "/" + spec.repo + "/releases/latest";

    httplib::Client api{"https://api.github.com"};
    api.set_connection_timeout(15);
    api.set_read_timeout(15);
    api.set_follow_location(true);
    ApplyCaCertPath(api);

    const httplib::Headers api_headers{
        {"User-Agent", "citron"},
        {"Accept", "application/vnd.github+json"},
    };
    const std::string releases_path =
        std::string{"/repos/"} + spec.owner + "/" + spec.repo + "/releases/latest";
    const auto api_result = api.Get(releases_path, api_headers);

    if (!api_result) {
        result.error = "network error contacting GitHub API";
        LOG_ERROR(WebService, "SSBU mod fetch ({}): {} failed, httplib error={}", spec.display_name,
                  releases_path, httplib::to_string(api_result.error()));
        return result;
    }
    if (api_result->status != 200) {
        const auto remaining_it = api_result->headers.find("X-RateLimit-Remaining");
        if (api_result->status == 403 && remaining_it != api_result->headers.end() &&
            remaining_it->second == "0") {
            result.error = "GitHub API rate limit exceeded -- try again later";
        } else {
            result.error =
                "GitHub API returned HTTP " + std::to_string(api_result->status);
        }
        LOG_ERROR(WebService, "SSBU mod fetch ({}): {}", spec.display_name, result.error);
        return result;
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(api_result->body);
    } catch (const nlohmann::json::exception& e) {
        result.error = std::string{"failed to parse GitHub API response: "} + e.what();
        LOG_ERROR(WebService, "SSBU mod fetch ({}): {}", spec.display_name, result.error);
        return result;
    }

    std::string download_url;
    std::string asset_name;
    std::size_t expected_size = 0;
    for (const auto& asset : json.value("assets", nlohmann::json::array())) {
        const std::string name = asset.value("name", "");
        if (spec.matches(name)) {
            download_url = asset.value("browser_download_url", "");
            asset_name = name;
            expected_size = asset.value("size", std::size_t{0});
            break;
        }
    }
    if (download_url.empty()) {
        result.error = "expected asset not found in latest release (may have been renamed upstream)";
        LOG_ERROR(WebService, "SSBU mod fetch ({}): {}", spec.display_name, result.error);
        return result;
    }

    const auto [dl_host, dl_path] = SplitUrl(download_url);
    if (dl_host.empty()) {
        result.error = "malformed asset download URL";
        return result;
    }

    // The signed redirect target intermittently 403s or truncates mid-transfer, worse for
    // larger assets; retry with backoff and verify against the size GitHub already told us.
    //
    // The asset redirect lands on GitHub's release-asset CDN (S3/Azure blob storage), which
    // runs bot-detection that consistently 403s the non-browser "citron" UA used for the API
    // request above -- confirmed by the response body being a fixed-size error page on every
    // retry rather than a growing/truncated download. A browser-like UA avoids that filter.
    httplib::Result dl_result;
    bool size_ok = false;
    constexpr int kMaxAttempts = 5;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        httplib::Client dl{dl_host};
        dl.set_connection_timeout(30);
        dl.set_read_timeout(30);
        dl.set_follow_location(true);
        ApplyCaCertPath(dl);
        dl_result = dl.Get(
            dl_path, httplib::Headers{
                         {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                        "AppleWebKit/537.36 (KHTML, like Gecko) "
                                        "Chrome/131.0.0.0 Safari/537.36"},
                         {"Accept", "*/*"}});
        size_ok = dl_result && (expected_size == 0 || dl_result->body.size() == expected_size);
        if (dl_result && dl_result->status == 200 && size_ok) {
            break;
        }
        if (attempt < kMaxAttempts - 1) {
            LOG_WARNING(WebService,
                       "SSBU mod fetch ({}): attempt {} failed (HTTP {}, {} of {} bytes), retrying",
                       spec.display_name, attempt + 1, dl_result ? dl_result->status : 0,
                       dl_result ? dl_result->body.size() : 0, expected_size);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 << attempt));
        }
    }

    if (!dl_result || dl_result->status != 200) {
        result.error = "failed to download asset (HTTP " +
                       std::to_string(dl_result ? dl_result->status : 0) + ")";
        LOG_ERROR(WebService, "SSBU mod fetch ({}): {}", spec.display_name, result.error);
        return result;
    }
    if (!size_ok) {
        result.error = "downloaded asset was truncated (" + std::to_string(dl_result->body.size()) +
                       " of " + std::to_string(expected_size) + " bytes)";
        LOG_ERROR(WebService, "SSBU mod fetch ({}): {}", spec.display_name, result.error);
        return result;
    }

    result.success = true;
    result.filename = asset_name;
    result.is_zip = asset_name.ends_with(".zip");
    result.data.assign(dl_result->body.begin(), dl_result->body.end());
    return result;
}

} // namespace

std::vector<ModFetchResult> FetchAllSkylineModAssets() {
    std::vector<ModFetchResult> results;
    results.reserve(kSkylineMods.size());
    for (const auto& spec : kSkylineMods) {
        results.push_back(FetchOne(spec));
    }
    return results;
}

} // namespace WebService::SkylineMods
