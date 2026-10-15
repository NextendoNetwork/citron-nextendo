// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace Service::SSL {

// Only call for the unauthenticated ctest /v1/ip response. Preserve the
// returned address; bridge the field name expected by the local HOME SDK.
inline std::optional<std::string> NormalizeCtestIpResponse(std::string_view response) {
    const auto split = response.find("\r\n\r\n");
    if (split == std::string_view::npos || !response.starts_with("HTTP/1.1 200 ")) {
        return std::nullopt;
    }
    const auto body = response.substr(split + 4);
    auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object() || json.contains("global_ip") || !json.contains("ip") ||
        !json["ip"].is_string()) {
        return std::nullopt;
    }
    std::string headers(response.substr(0, split));
    const auto length_start = headers.find("\r\nContent-Length:");
    const auto lower_length_start = headers.find("\r\ncontent-length:");
    const auto start = length_start != std::string::npos ? length_start : lower_length_start;
    if (start == std::string::npos || headers.find("Transfer-Encoding:") != std::string::npos ||
        headers.find("transfer-encoding:") != std::string::npos) {
        return std::nullopt;
    }
    const auto colon = headers.find(':', start);
    const auto end = headers.find("\r\n", colon);
    const auto length = headers.substr(colon + 1, end == std::string::npos ? end : end - colon - 1);
    size_t declared{};
    bool digit_seen{};
    for (char ch : length) {
        if (ch == ' ' || ch == '\t') continue;
        if (ch < '0' || ch > '9' || declared > 65536) return std::nullopt;
        declared = declared * 10 + static_cast<size_t>(ch - '0');
        digit_seen = true;
    }
    if (!digit_seen || declared != body.size()) return std::nullopt;
    json["global_ip"] = json["ip"];
    json.erase("ip");
    const auto normalized_body = json.dump();
    headers.replace(colon + 1, end == std::string::npos ? headers.size() - colon - 1 :
                    end - colon - 1, " " + std::to_string(normalized_body.size()));
    return headers + "\r\n\r\n" + normalized_body;
}
} // namespace Service::SSL
