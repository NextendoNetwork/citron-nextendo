// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509_vfy.h>

#include "common/hex_util.h"
#include "common/logging.h"
#include "common/nextendo_account.h"
#include "common/string_util.h"
#include "web_service/nextendo_api.h"

namespace WebService::NextendoApi {

namespace {

constexpr const char* CanonicalUrl = "https://nextendo.network";
constexpr const char* ClientId = "nextendo-emulator";
constexpr int TimeoutSeconds = 15;

struct Callback {
    std::mutex mutex;
    std::condition_variable cv;
    bool received = false;
    std::string code;
    std::string state;
    std::string error;
};

std::string Base64Url(std::span<const u8> data) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    for (std::size_t i = 0; i < data.size(); i += 3) {
        const auto remaining = data.size() - i;
        const u32 triple = (static_cast<u32>(data[i]) << 16) |
                           (remaining > 1 ? static_cast<u32>(data[i + 1]) << 8 : 0) |
                           (remaining > 2 ? static_cast<u32>(data[i + 2]) : 0);

        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        if (remaining > 1) {
            out += alphabet[(triple >> 6) & 0x3F];
        }
        if (remaining > 2) {
            out += alphabet[triple & 0x3F];
        }
    }

    return out;
}

// Standard (RFC 4648, padded) base64 -- distinct from Base64Url above. The presence AppField is
// an opaque BINARY blob (Nintendo's packed nn::friends AppKeyValueStorage), not text: handing raw
// bytes to nlohmann::json::dump() throws type_error.316 the moment the blob contains a byte
// sequence that isn't valid UTF-8, which packed binary reliably does. The account server stores
// this string opaquely and the Ryujinx fork encodes/decodes it with .NET's Convert.ToBase64String
// (standard alphabet), so this has to match that, not the URL-safe alphabet used for PKCE.
std::string Base64StdEncode(std::span<const u8> data) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const u32 triple = (static_cast<u32>(data[i]) << 16) |
                           (static_cast<u32>(data[i + 1]) << 8) | static_cast<u32>(data[i + 2]);
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += alphabet[(triple >> 6) & 0x3F];
        out += alphabet[triple & 0x3F];
    }
    const auto remaining = data.size() - i;
    if (remaining == 1) {
        const u32 triple = static_cast<u32>(data[i]) << 16;
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += "==";
    } else if (remaining == 2) {
        const u32 triple =
            (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8);
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += alphabet[(triple >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

std::vector<u8> Base64StdDecode(std::string_view text) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<s8, 256> table;
    table.fill(-1);
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        table[static_cast<u8>(alphabet[i])] = static_cast<s8>(i);
    }

    std::vector<u8> out;
    out.reserve(text.size() / 4 * 3);

    u32 buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        const s8 value = table[static_cast<u8>(c)];
        if (value < 0) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<u32>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::vector<u8> Sha256(std::string_view text) {
    std::vector<u8> digest(SHA256_DIGEST_LENGTH);
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest.data());
    return digest;
}

std::string RandomUrlSafe(std::size_t bytes) {
    std::vector<u8> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        return {};
    }
    return Base64Url(buffer);
}

std::string PercentEncode(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out += c;
        } else {
            out += fmt::format("%{:02X}", static_cast<unsigned char>(c));
        }
    }
    return out;
}

std::string LoopbackPage(bool ok) {
    const std::string inner =
        ok ? "<h1 style='color:#33e86b'>Signed in</h1><p>You can close this tab and go back to "
             "citron.</p>"
           : "<h1 style='color:#ff8a8a'>Sign-in cancelled</h1><p>Go back to citron and try "
             "again.</p>";
    return "<!doctype html><meta charset=utf-8><title>Nextendo</title>"
           "<body style='font-family:system-ui,sans-serif;background:#0f1115;color:#e7e9ee;"
           "display:grid;place-items:center;height:100vh;margin:0'>"
           "<div style='text-align:center;max-width:420px'>" +
           inner + "</div>";
}

bool IsLoopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
}

// CMakeModules/openssl_build.cmake links web_service against a static OpenSSL built from source
// (originally for Windows cross-compilation, but it's wired up unconditionally on every
// platform). X509_get_default_cert_file()/_dir() are compile-time macros baked into that vendored
// copy's headers, so they resolve to a path inside the build tree that was never populated with
// real certs -- not the system's actual CA store. On Linux, point at the real distro-provided
// bundle directly. On Windows/macOS there's no equivalent fixed path; leave the client's cert
// path unset so httplib falls through to its own native cert-store loader for those platforms.
void ApplyCaCertPath(httplib::Client& client) {
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
    LOG_ERROR(WebService, "ApplyCaCertPath: no known system CA bundle found");
#else
    (void)client;
#endif
}

// Splits "https://host:port" into a host root httplib accepts, plus validates the scheme. The
// account token rides on these requests, so an arbitrary override must not be able to receive it.
std::optional<std::string> SanitizeBaseUrl(std::string raw) {
    raw = Common::StripSpaces(raw);
    while (!raw.empty() && raw.back() == '/') {
        raw.pop_back();
    }
    if (raw.empty()) {
        return std::nullopt;
    }

    const auto scheme_end = raw.find("://");
    if (scheme_end == std::string::npos) {
        return std::nullopt;
    }
    const std::string scheme = raw.substr(0, scheme_end);
    std::string authority = raw.substr(scheme_end + 3);
    if (authority.empty() || authority.find('/') != std::string::npos) {
        return std::nullopt;
    }

    std::string host = authority;
    if (const auto colon = host.rfind(':'); colon != std::string::npos && host.back() != ']') {
        host = host.substr(0, colon);
    }

    if (IsLoopback(host)) {
        return raw;
    }
    if (scheme != "https") {
        return std::nullopt;
    }
    if (host != "nextendo.network" && !host.ends_with(".nextendo.network")) {
        return std::nullopt;
    }
    return raw;
}

httplib::Client& SharedClient() {
    static httplib::Client client = [] {
        httplib::Client c{BaseUrl()};
        c.set_connection_timeout(TimeoutSeconds);
        c.set_read_timeout(TimeoutSeconds);
        c.set_follow_location(true);
        c.set_keep_alive(true);
        ApplyCaCertPath(c);
        return c;
    }();
    return client;
}

httplib::Result Send(const std::string& method, const std::string& path, const std::string& body,
                     const std::string& bearer, const httplib::Headers& extra_headers = {}) {
    static std::mutex client_mutex;
    std::lock_guard lock{client_mutex};
    httplib::Client& client = SharedClient();

    httplib::Headers headers{{"User-Agent", "citron"}};
    if (!bearer.empty()) {
        headers.emplace("Authorization", "Bearer " + bearer);
    }
    for (const auto& [key, value] : extra_headers) {
        headers.emplace(key, value);
    }

    auto result = method == "GET"    ? client.Get(path, headers)
                 : method == "PUT"   ? client.Put(path, headers, body, "application/json")
                                     : client.Post(path, headers, body, "application/json");
    if (!result) {
        const long verify_result = client.get_openssl_verify_result();
        LOG_ERROR(WebService, "Send {} {}: httplib error={}, openssl verify_result={} ({})",
                  method, path, httplib::to_string(result.error()), verify_result,
                  X509_verify_cert_error_string(verify_result));
    }
    return result;
}

// A 401 on an authenticated call means the stored token is expired or revoked. Left in place it
// keeps presenting an identity that is no longer ours, so drop the session and fall back to the
// anonymous stub. Only a real 401 counts: network errors and 5xx must never clear a good session.
bool ClearSessionIfRejected(const httplib::Result& result) {
    if (!result || result->status != 401) {
        return false;
    }
    LOG_WARNING(WebService, "Nextendo rejected the stored account token; signing out");
    Common::NextendoAccount::Clear();
    return true;
}

// The server answers failures with {"error": "..."}.
std::string ErrorFrom(const std::string& payload, const std::string& fallback) {
    try {
        const auto json = nlohmann::json::parse(payload);
        if (json.contains("error") && json["error"].is_string()) {
            return json["error"].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
    }
    return fallback;
}

} // Anonymous namespace

std::string BaseUrl() {
    static const std::string url = [] {
        const char* env = std::getenv("NEXTENDO_API");
        if (env && *env) {
            if (const auto sanitized = SanitizeBaseUrl(env)) {
                return *sanitized;
            }
            LOG_WARNING(WebService,
                        "Ignoring NEXTENDO_API=\"{}\": only https on nextendo.network, or "
                        "loopback, may receive the account token",
                        env);
        }
        return std::string{CanonicalUrl};
    }();

    return url;
}

LoginResult SignInWithBrowser(const std::function<void(const std::string&)>& open_url) {
    LoginResult out;

    const std::string verifier = RandomUrlSafe(32);
    const std::string challenge = Base64Url(Sha256(verifier));
    const std::string state = RandomUrlSafe(24);

    Callback callback;
    httplib::Server server;

    server.Get("/callback", [&](const httplib::Request& request, httplib::Response& response) {
        {
            std::lock_guard lock{callback.mutex};
            callback.code = request.get_param_value("code");
            callback.state = request.get_param_value("state");
            callback.error = request.get_param_value("error");
            callback.received = true;
        }
        const bool ok = callback.error.empty() && !callback.code.empty();
        response.set_content(LoopbackPage(ok), "text/html; charset=utf-8");
        callback.cv.notify_all();
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    if (port < 0) {
        out.error = "Could not open a local port for the sign-in callback.";
        LOG_ERROR(WebService, "SignInWithBrowser: bind_to_any_port failed");
        return out;
    }

    std::thread listener{[&server] { void(server.listen_after_bind()); }};
    const std::string redirect_uri = fmt::format("http://127.0.0.1:{}/callback", port);

    const std::string auth_url =
        fmt::format("{}/api/oauth/authorize?response_type=code&client_id={}"
                    "&redirect_uri={}&scope=identity+friends&state={}"
                    "&code_challenge={}&code_challenge_method=S256",
                    BaseUrl(), ClientId, PercentEncode(redirect_uri), state, challenge);
    LOG_INFO(WebService, "SignInWithBrowser: opening {}", auth_url);
    open_url(auth_url);

    {
        std::unique_lock lock{callback.mutex};
        callback.cv.wait_for(lock, std::chrono::minutes{5},
                             [&callback] { return callback.received; });
    }

    server.stop();
    listener.join();

    if (!callback.received) {
        out.error = "Sign-in timed out.";
        LOG_ERROR(WebService, "SignInWithBrowser: no callback received within 5 minutes");
        return out;
    }
    if (!callback.error.empty()) {
        out.error = callback.error == "access_denied" ? "Sign-in was declined." : callback.error;
        LOG_ERROR(WebService, "SignInWithBrowser: callback returned error={}", callback.error);
        return out;
    }
    if (callback.code.empty()) {
        out.error = "The browser returned no authorization code.";
        LOG_ERROR(WebService, "SignInWithBrowser: callback had no code");
        return out;
    }
    if (callback.state != state) {
        out.error = "Anti-CSRF check failed; the sign-in was not completed.";
        LOG_ERROR(WebService, "SignInWithBrowser: state mismatch, expected={} got={}", state,
                  callback.state);
        return out;
    }
    LOG_INFO(WebService, "SignInWithBrowser: callback received, exchanging code for token");

    const httplib::Params form{
        {"grant_type", "authorization_code"}, {"code", callback.code},
        {"client_id", ClientId},              {"redirect_uri", redirect_uri},
        {"code_verifier", verifier},
    };

    httplib::Client client{BaseUrl()};
    client.set_connection_timeout(TimeoutSeconds);
    client.set_read_timeout(TimeoutSeconds);
    client.set_follow_location(true);
    ApplyCaCertPath(client);

    const auto result = client.Post("/api/oauth/token", form);
    if (!result) {
        out.error = "Could not reach the Nextendo account server.";
        const long verify_result = client.get_openssl_verify_result();
        LOG_ERROR(WebService,
                  "SignInWithBrowser: token exchange had no response (httplib error={}, "
                  "openssl verify_result={} [{}])",
                  httplib::to_string(result.error()), verify_result,
                  X509_verify_cert_error_string(verify_result));
        return out;
    }
    if (result->status != 200) {
        out.error = ErrorFrom(result->body, "Could not exchange the authorization code.");
        LOG_ERROR(WebService, "SignInWithBrowser: token exchange failed (HTTP {}): {}",
                  result->status, result->body);
        return out;
    }

    try {
        const auto json = nlohmann::json::parse(result->body);
        const auto& account = json.at("account");

        out.pid = account.at("pid").get<u64>();
        out.username = account.value("username", std::string{});
        out.friend_code = account.value("friend_code", std::string{});
        out.token = json.value("nex_token", std::string{});

        if (out.pid == 0 || out.token.empty()) {
            out.error = "The account server returned an incomplete sign-in.";
            LOG_ERROR(WebService, "SignInWithBrowser: incomplete sign-in, pid={} token_empty={}",
                      out.pid, out.token.empty());
            return out;
        }
        out.ok = true;
        LOG_INFO(WebService, "SignInWithBrowser: signed in as pid={} username={}", out.pid,
                 out.username);
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected sign-in response: {}", e.what());
        LOG_ERROR(WebService, "SignInWithBrowser: failed to parse token response: {} (body={})",
                  e.what(), result->body);
    }

    return out;
}

OnlineStatus GetOnlineStatus() {
    OnlineStatus out;

    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        return out;
    }

    const auto result = Send("GET", "/api/online-status", {}, token);
    if (ClearSessionIfRejected(result) || !result || result->status != 200) {
        return out;
    }

    try {
        const auto json = nlohmann::json::parse(result->body);
        out.allow = json.value("allow", false);
        out.reason = json.value("reason", std::string{});
        out.message = json.value("message", std::string{});
        out.queried = true;
    } catch (const nlohmann::json::exception&) {
    }

    return out;
}



std::vector<u8> DownloadBcatSeed(const std::string& title_id_hex) {
    const std::string token = Common::NextendoAccount::GetToken();
    const auto result = Send("GET", "/api/bcat/" + title_id_hex, {}, token);

    if (ClearSessionIfRejected(result)) {
        return {};
    }
    if (!result || result->status != 200) {
        LOG_WARNING(WebService, "Nextendo BCAT seed download failed (HTTP {})",
                    result ? result->status : 0);
        return {};
    }

    return std::vector<u8>(result->body.begin(), result->body.end());
}

std::string HashBcatSeedHex(const std::vector<u8>& data) {
    const auto digest = Sha256(std::string_view{reinterpret_cast<const char*>(data.data()),
                                                 data.size()});
    return Common::HexToString(digest, false);
}

std::optional<std::vector<u8>> PullSave(const std::string& title_id_hex) {
    const std::string token = Common::NextendoAccount::GetToken();
    const auto result = Send("GET", "/api/save/" + title_id_hex, {}, token);

    if (ClearSessionIfRejected(result)) {
        return std::nullopt;
    }
    if (!result || result->status == 204) {
        return std::nullopt; // no cloud save stored yet
    }
    if (result->status != 200) {
        LOG_WARNING(WebService, "Nextendo save pull failed (HTTP {})", result->status);
        return std::nullopt;
    }

    return std::vector<u8>(result->body.begin(), result->body.end());
}

std::string PushSave(const std::string& title_id_hex, std::span<const u8> data) {
    const std::string token = Common::NextendoAccount::GetToken();
    const std::string body(reinterpret_cast<const char*>(data.data()), data.size());
    const auto result = Send("POST", "/api/save/" + title_id_hex, body, token);

    if (ClearSessionIfRejected(result)) {
        return "Session expired.";
    }
    if (!result) {
        return "Could not reach the Nextendo account server.";
    }
    if (result->status != 200) {
        return ErrorFrom(result->body, "Could not upload the save.");
    }
    return {};
}

std::map<std::string, int> GetOnlineCounts() {
    std::map<std::string, int> out;

    const auto result = Send("GET", "/api/online-counts", {}, {});
    if (!result || result->status != 200) {
        LOG_WARNING(WebService, "Nextendo online-counts fetch failed (HTTP {})",
                    result ? result->status : 0);
        return out;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        if (const auto counts = json.find("counts"); counts != json.end() && counts->is_object()) {
            for (const auto& [title_id, count] : counts->items()) {
                if (count.is_number_integer()) {
                    out.emplace(title_id, count.get<int>());
                }
            }
        }
    } catch (const nlohmann::json::exception& e) {
        LOG_WARNING(WebService, "Unexpected online-counts response: {}", e.what());
    }

    return out;
}

int GetNzpOnlineCount() {
    const char* env = std::getenv("NZP_API"); // test VPS by default
    const std::string base_url = (env && *env) ? env : "http://144.202.45.50:8090";

    httplib::Client client{base_url};
    client.set_connection_timeout(3);
    client.set_read_timeout(3);
    const auto result = client.Get("/online-count");
    if (!result || result->status != 200) {
        return 0;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        if (const auto it = json.find("count"); it != json.end() && it->is_number_integer()) {
            return it->get<int>();
        }
    } catch (const nlohmann::json::exception& e) {
        LOG_WARNING(WebService, "Unexpected nzp online-count response: {}", e.what());
    }
    return 0;
}

Profile GetProfile() {
    Profile out;

    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        out.error = "Not signed in.";
        return out;
    }

    const auto result = Send("GET", "/api/profile", {}, token);
    if (ClearSessionIfRejected(result)) {
        out.error = "Your session expired. Sign in again.";
        return out;
    }
    if (!result) {
        out.error = "Could not reach the Nextendo account server.";
        return out;
    }
    if (result->status != 200) {
        out.error =
            ErrorFrom(result->body, fmt::format("Could not load profile (HTTP {}).", result->status));
        return out;
    }

    try {
        const auto json = nlohmann::json::parse(result->body);
        out.name = json.value("username", std::string{});
        if (const auto profile = json.find("profile"); profile != json.end() && !profile->is_null()) {
            out.image_base64 = profile->value("image", std::string{});
            out.console_nickname = profile->value("name", std::string{});
            out.mii_base64 = profile->value("mii", std::string{});
            out.avatar_id = profile->value("avatar", std::string{});
            out.color_hex = profile->value("color", std::string{});
        }
        out.ok = true;
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected profile response: {}", e.what());
    }

    return out;
}

std::string PushProfilePicture(const std::string& image_base64) {
    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        return "Not signed in.";
    }

    const Profile current = GetProfile();
    if (!current.ok) {
        return current.error.empty() ? "Could not load your current profile." : current.error;
    }

    nlohmann::json body{{"image", image_base64}};
    if (!current.console_nickname.empty()) {
        body["name"] = current.console_nickname;
    }
    if (!current.mii_base64.empty()) {
        body["mii"] = current.mii_base64;
    }
    if (!current.avatar_id.empty()) {
        body["avatar"] = current.avatar_id;
    }
    if (!current.color_hex.empty()) {
        body["color"] = current.color_hex;
    }

    const auto result = Send("POST", "/api/profile", body.dump(), token);
    if (ClearSessionIfRejected(result)) {
        return "Your session expired. Sign in again.";
    }
    if (!result || result->status != 200) {
        return ErrorFrom(result ? result->body : std::string{},
                         fmt::format("Could not update your profile picture (HTTP {}).",
                                     result ? result->status : 0));
    }
    return {};
}

std::string SetUsername(const std::string& username) {
    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        return "Not signed in.";
    }

    const nlohmann::json body{{"Username", username}};
    const auto result = Send("PUT", "/api/username", body.dump(), token);
    if (ClearSessionIfRejected(result)) {
        return "Your session expired. Sign in again.";
    }
    if (!result || result->status != 200) {
        return ErrorFrom(result ? result->body : std::string{},
                         fmt::format("Could not change your username (HTTP {}).",
                                     result ? result->status : 0));
    }
    return {};
}

HistoryList GetHistory() {
    HistoryList out;

    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        out.error = "Not signed in.";
        return out;
    }

    const auto result = Send("GET", "/api/history", {}, token);
    if (ClearSessionIfRejected(result)) {
        out.error = "Your session expired. Sign in again.";
        return out;
    }
    if (!result) {
        out.error = "Could not reach the Nextendo account server.";
        return out;
    }
    if (result->status != 200) {
        out.error =
            ErrorFrom(result->body, fmt::format("Could not load history (HTTP {}).", result->status));
        return out;
    }

    try {
        const auto json = nlohmann::json::parse(result->body);
        for (const auto& entry : json.value("history", nlohmann::json::array())) {
            HistoryItem item;
            item.title_id = entry.value("title_id", std::string{});
            item.name = entry.value("name", std::string{});
            item.icon_base64 = entry.value("icon", std::string{});
            item.seconds = entry.value("seconds", u64{0});
            item.last_played = entry.value("last_played", std::string{});
            out.entries.push_back(std::move(item));
        }
        std::sort(out.entries.begin(), out.entries.end(), [](const auto& a, const auto& b) {
            return a.last_played > b.last_played;
        });
        out.ok = true;
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected history response: {}", e.what());
    }

    return out;
}

void SyncHistory(const std::vector<HistoryEntry>& entries) {
    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty() || entries.empty()) {
        return;
    }

    nlohmann::json history = nlohmann::json::array();
    for (const auto& entry : entries) {
        nlohmann::json item{
            {"title_id", entry.title_id},
            {"seconds", entry.seconds},
            {"last_played", entry.last_played},
        };
        if (!entry.name.empty()) {
            item["name"] = entry.name;
        }
        if (!entry.icon_base64.empty()) {
            item["icon"] = entry.icon_base64;
        }
        history.push_back(std::move(item));
    }

    httplib::Client client{BaseUrl()};
    client.set_connection_timeout(TimeoutSeconds);
    client.set_read_timeout(TimeoutSeconds);
    client.set_follow_location(true);
    ApplyCaCertPath(client);

    httplib::Headers headers{{"User-Agent", "citron"}, {"Authorization", "Bearer " + token}};
    const auto result = client.Put("/api/history", headers,
                                   nlohmann::json{{"history", history}}.dump(), "application/json");

    if (ClearSessionIfRejected(result)) {
        return;
    }
    if (!result || result->status != 200) {
        LOG_WARNING(WebService, "Nextendo history sync failed (HTTP {})",
                    result ? result->status : 0);
        return;
    }
    LOG_INFO(WebService, "Nextendo history synced ({} title(s))", entries.size());
}

namespace {

Friend ParseFriend(const nlohmann::json& json) {
    Friend out;
    out.pid = json.value("pid", u64{0});
    out.name = json.value("name", std::string{});
    if (out.name.empty()) {
        out.name = json.value("username", std::string{});
    }
    out.friend_code = json.value("friend_code", std::string{});
    out.image_base64 = json.value("image", std::string{});
    if (const auto presence = json.find("presence"); presence != json.end()) {
        out.presence_status = presence->value("status", s32{0});
        // The server hands back what the host published: base64 text wrapping the raw
        // AppKeyValueStorage blob (see Base64StdEncode in PushPresence). Decode it back to the
        // raw bytes the guest's UserPresenceImpl.app_key_value expects.
        const auto app_field_b64 = presence->value("app_field", std::string{});
        const auto decoded = Base64StdDecode(app_field_b64);
        out.app_field = std::string{reinterpret_cast<const char*>(decoded.data()), decoded.size()};
        out.app_id = presence->value("app_id", std::string{});
        out.app_name = presence->value("app_name", std::string{});
    }
    // [Nextendo] TEMP: unconditional (not gated on "presence" being present) so we can see
    // whether name/username ever actually comes back non-empty from the account server, or
    // whether the Friends viewer's blank tiles are a server-data gap rather than a client bug.
    LOG_DEBUG(WebService, "[Nextendo] ParseFriend pid={} name='{}' status={} app_field={}",
             out.pid, out.name, out.presence_status, Common::HexToString(
                 std::vector<u8>(out.app_field.begin(), out.app_field.end()), false));
    return out;
}

LobbyPlayer ParseLobbyPlayer(const nlohmann::json& json) {
    LobbyPlayer out;
    out.pid = json.value("pid", u64{0});
    out.name = json.value("name", std::string{});
    out.known = json.value("known", false);
    out.avatar_url = json.value("avatar_url", std::string{});
    out.friend_code = json.value("friend_code", std::string{});
    out.host = json.value("host", false);
    out.is_me = json.value("is_me", false);
    out.title_id = json.value("title_id", std::string{});
    out.seen_at = json.value("seen_at", std::string{});
    return out;
}

// Shared shape for the mutation endpoints: POST {pid}, empty string means success.
std::string PostPid(const std::string& path, u64 pid) {
    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        return "Not signed in.";
    }
    const auto result = Send("POST", path, nlohmann::json{{"pid", pid}}.dump(), token);
    if (ClearSessionIfRejected(result)) {
        return "Your session expired. Sign in again.";
    }
    if (!result) {
        return "Could not reach the Nextendo account server.";
    }
    if (result->status != 200) {
        return ErrorFrom(result->body, fmt::format("Request failed (HTTP {}).", result->status));
    }
    return {};
}

} // Anonymous namespace

FriendList GetFriends() {
    FriendList out;

    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        out.error = "Not signed in.";
        return out;
    }

    const auto result = Send("GET", "/api/friends", {}, token);
    if (ClearSessionIfRejected(result)) {
        out.error = "Your session expired. Sign in again.";
        LOG_WARNING(WebService, "GetFriends: {}", out.error);
        return out;
    }
    if (!result) {
        out.error = "Could not reach the Nextendo account server.";
        LOG_WARNING(WebService, "GetFriends: {}", out.error);
        return out;
    }
    if (result->status != 200) {
        out.error = ErrorFrom(result->body, fmt::format("Could not load friends (HTTP {}).",
                                                       result->status));
        LOG_WARNING(WebService, "GetFriends: {}", out.error);
        return out;
    }

    try {
        const auto json = nlohmann::json::parse(result->body);
        for (const auto& entry : json.value("friends", nlohmann::json::array())) {
            out.friends.push_back(ParseFriend(entry));
        }
        for (const auto& entry : json.value("requests", nlohmann::json::array())) {
            out.requests.push_back(ParseFriend(entry));
        }
        out.ok = true;
        LOG_INFO(WebService, "GetFriends: {} friend(s), {} request(s)", out.friends.size(),
                 out.requests.size());
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected friends response: {}", e.what());
        LOG_WARNING(WebService, "GetFriends: {}", out.error);
    }

    return out;
}

std::string AddFriendByCode(const std::string& friend_code) {
    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        return "Not signed in.";
    }
    const auto result =
        Send("POST", "/api/friends", nlohmann::json{{"friend_code", friend_code}}.dump(), token);
    if (ClearSessionIfRejected(result)) {
        return "Your session expired. Sign in again.";
    }
    if (!result) {
        return "Could not reach the Nextendo account server.";
    }
    if (result->status != 200) {
        return ErrorFrom(result->body, fmt::format("Request failed (HTTP {}).", result->status));
    }
    return {};
}

std::string AcceptFriend(u64 pid) {
    return PostPid("/api/friends/accept", pid);
}

std::string DeclineFriend(u64 pid) {
    return PostPid("/api/friends/decline", pid);
}

std::string RemoveFriend(u64 pid) {
    return PostPid("/api/friends/remove", pid);
}

void PushProfileName(const std::string& name) {
    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty() || name.empty()) {
        return;
    }
    const auto result =
        Send("POST", "/api/profile", nlohmann::json{{"name", name}}.dump(), token);
    if (ClearSessionIfRejected(result)) {
        return;
    }
    if (!result || result->status != 200) {
        LOG_WARNING(WebService, "Nextendo profile name push failed (HTTP {})",
                    result ? result->status : 0);
    }
}

void PushPresence(s32 status, const std::string& app_field, const std::string& app_id,
                  const std::string& app_name) {
    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        return;
    }

    // app_field is the raw binary AppKeyValueStorage blob (SessionId/Mode/Full/InGame/...), not
    // text -- nlohmann::json::dump() throws type_error.316 the moment it contains a byte that
    // isn't valid UTF-8, which packed binary reliably does. Base64 it first, matching what the
    // server documents and what the Ryujinx fork sends.
    const std::string encoded_app_field = Base64StdEncode(
        std::span<const u8>{reinterpret_cast<const u8*>(app_field.data()), app_field.size()});
    const std::string body = nlohmann::json{{"status", status},
                                            {"app_field", encoded_app_field},
                                            {"app_id", app_id},
                                            {"app_name", app_name},
                                            {"app_detail", ""}}
                                 .dump();
    const auto result = Send("POST", "/api/presence", body, token);
    if (ClearSessionIfRejected(result)) {
        return;
    }
    if (!result || result->status != 200) {
        LOG_WARNING(WebService, "Nextendo presence push failed (HTTP {})",
                    result ? result->status : 0);
        return;
    }
    LOG_DEBUG(WebService, "Nextendo presence published (status={})", status);
}

Lobby GetMyLobby() {
    Lobby out;

    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        return out;
    }

    const auto result = Send("GET", "/api/my-lobby", {}, token);
    if (ClearSessionIfRejected(result) || !result || result->status != 200) {
        return out;
    }

    try {
        const auto json = nlohmann::json::parse(result->body);
        out.in_lobby = json.value("in_lobby", false);
        if (!out.in_lobby) {
            return out;
        }
        out.title_id = json.value("title_id", std::string{});
        if (const auto lobby = json.find("lobby"); lobby != json.end()) {
            out.type = lobby->value("type", std::string{});
            out.state = lobby->value("state", std::string{});
            out.state_code = lobby->value("state_code", std::string{});
            out.id = lobby->value("id", u64{0});
            out.count = lobby->value("count", 0);
            out.max = lobby->value("max", 0);
        }
        for (const auto& entry : json.value("players", nlohmann::json::array())) {
            out.players.push_back(ParseLobbyPlayer(entry));
        }
    } catch (const nlohmann::json::exception& e) {
        LOG_WARNING(WebService, "GetMyLobby: {}", e.what());
        out = {};
    }

    return out;
}

std::vector<LobbyPlayer> GetRecentPlayers() {
    std::vector<LobbyPlayer> out;

    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        return out;
    }

    const auto result = Send("GET", "/api/recent-players", {}, token);
    if (ClearSessionIfRejected(result) || !result || result->status != 200) {
        return out;
    }

    try {
        const auto json = nlohmann::json::parse(result->body);
        for (const auto& entry : json.value("players", nlohmann::json::array())) {
            out.push_back(ParseLobbyPlayer(entry));
        }
    } catch (const nlohmann::json::exception& e) {
        LOG_WARNING(WebService, "GetRecentPlayers: {}", e.what());
    }

    return out;
}

std::string GetAvatarByPid(u64 pid) {
    if (pid == 0) {
        return {};
    }
    // No bearer: /api/avatar is public, and the token must never leave with a request whose
    // target could be influenced by server-supplied data. The path is built from pid alone.
    const auto result = Send("GET", fmt::format("/api/avatar?pid={}", pid), {}, {});
    if (!result || result->status != 200) {
        return {};
    }
    return Base64StdEncode(
        std::span<const u8>{reinterpret_cast<const u8*>(result->body.data()), result->body.size()});
}

std::string ReportPlayer(u64 pid, const std::string& reason, const std::string& comment) {
    const std::string token = Common::NextendoAccount::GetToken();
    if (token.empty()) {
        return "Not signed in.";
    }
    const std::string body =
        nlohmann::json{{"target_pid", pid}, {"reason", reason}, {"comment", comment}}.dump();
    const auto result = Send("POST", "/api/report-player", body, token);
    if (ClearSessionIfRejected(result)) {
        return "Your session expired. Sign in again.";
    }
    if (!result) {
        return "Could not reach the Nextendo account server.";
    }
    if (result->status != 200) {
        return ErrorFrom(result->body, fmt::format("Request failed (HTTP {}).", result->status));
    }
    return {};
}

std::optional<int> PingBackend() {
    const auto start = std::chrono::steady_clock::now();
    const auto result = Send("GET", "/api/health", {}, {});
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (!result || result->status != 200) {
        return std::nullopt;
    }
    return static_cast<int>(elapsed_ms.count());
}

} // namespace WebService::NextendoApi
