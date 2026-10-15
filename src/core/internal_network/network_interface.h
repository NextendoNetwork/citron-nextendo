// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>
#include "common/common_types.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

namespace Network {

struct NetworkInterface {
    std::string name;
    struct in_addr ip_address;
    struct in_addr subnet_mask;
    struct in_addr gateway;
    bool wireless{};
    std::string ssid;
    u8 signal_bars{};
    bool wireless_connection_available{};
    u32 wireless_authentication{};
    u32 wireless_cipher{};
    u32 frequency_khz{};
    u16 wireless_channel{};
    s32 rssi_dbm{};
    std::array<u8, 6> access_point_mac{};
    std::array<u8, 6> mac_address{};
    std::array<in_addr, 2> dns_servers{};
};

std::vector<NetworkInterface> GetAvailableNetworkInterfaces();
std::optional<NetworkInterface> GetSelectedNetworkInterface();
void SelectFirstNetworkInterface();

} // namespace Network
