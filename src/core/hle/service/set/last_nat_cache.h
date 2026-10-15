// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <cstdint>
#include <utility>

namespace Service::Set {

// Stores only a network fingerprint and the last completed HOME NAT classification.
// The original SSID, addresses, and account data are never written here.
class LastNatCache {
public:
    explicit LastNatCache(std::filesystem::path path_) : path(std::move(path_)) {}

    void Begin(std::optional<std::uint64_t> network) { pending_network = network; }

    bool Complete(unsigned nat, std::optional<std::uint64_t> network) {
        const auto started = std::exchange(pending_network, std::nullopt);
        if (nat >= 5 || !network || !started || network != started) return false;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return false;
        std::ofstream out(path, std::ios::trunc);
        out << "CITRON_NAT_1 " << *network << ' ' << nat << '\n';
        out.flush();
        return out.good();
    }

    std::string Display(std::optional<std::uint64_t> network) const {
        if (!network) return "--";
        std::ifstream in(path);
        std::string magic, extra;
        std::uint64_t stored{};
        unsigned nat{};
        if (!(in >> magic >> stored >> nat) || magic != "CITRON_NAT_1" || nat >= 5 ||
            (in >> extra)) return "--";
        if (stored != *network) return "retest";
        return std::string(1, "ABCDF"[nat]);
    }

private:
    std::filesystem::path path;
    std::optional<std::uint64_t> pending_network;
};
} // namespace Service::Set
