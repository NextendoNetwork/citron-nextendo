// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#include "common/bit_cast.h"
#include "common/common_types.h"
#include "common/logging.h"
#include <ranges>
#include "common/settings.h"
#include "common/string_util.h"
#include "core/internal_network/network_interface.h"

#ifdef _WIN32
#include <iphlpapi.h>
#include <wlanapi.h>
#else
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace Network {

#ifdef _WIN32

namespace {
void ReadWirelessConnection(const IP_ADAPTER_ADDRESSES& adapter, NetworkInterface& result) {
    if (!result.wireless) {
        return;
    }
    // Query only the connected network; no scan or saved Wi-Fi credentials.
    const auto library = LoadLibraryW(L"wlanapi.dll");
    if (!library) {
        return;
    }
    const auto open_handle = reinterpret_cast<decltype(&WlanOpenHandle)>(
        GetProcAddress(library, "WlanOpenHandle"));
    const auto query_interface = reinterpret_cast<decltype(&WlanQueryInterface)>(
        GetProcAddress(library, "WlanQueryInterface"));
    const auto free_memory = reinterpret_cast<decltype(&WlanFreeMemory)>(
        GetProcAddress(library, "WlanFreeMemory"));
    const auto close_handle = reinterpret_cast<decltype(&WlanCloseHandle)>(
        GetProcAddress(library, "WlanCloseHandle"));
    const auto get_bss_list = reinterpret_cast<decltype(&WlanGetNetworkBssList)>(
        GetProcAddress(library, "WlanGetNetworkBssList"));
    HANDLE client{};
    DWORD version{};
    GUID guid{};
    if (open_handle && query_interface && free_memory && close_handle &&
        ConvertInterfaceLuidToGuid(&adapter.Luid, &guid) == NO_ERROR &&
        open_handle(2, nullptr, &version, &client) == ERROR_SUCCESS) {
        DWORD size{};
        void* data{};
        WLAN_OPCODE_VALUE_TYPE value_type{};
        if (query_interface(client, &guid, wlan_intf_opcode_current_connection, nullptr,
                            &size, &data, &value_type) == ERROR_SUCCESS && data) {
            if (size >= sizeof(WLAN_CONNECTION_ATTRIBUTES)) {
                const auto& connection = *static_cast<WLAN_CONNECTION_ATTRIBUTES*>(data);
                if (connection.isState == wlan_interface_state_connected) {
                    const auto& association = connection.wlanAssociationAttributes;
                    result.wireless_connection_available = true;
                    result.wireless_authentication =
                        connection.wlanSecurityAttributes.dot11AuthAlgorithm;
                    result.wireless_cipher =
                        connection.wlanSecurityAttributes.dot11CipherAlgorithm;
                    std::copy_n(association.dot11Bssid, result.access_point_mac.size(),
                                result.access_point_mac.begin());
                    result.ssid.assign(
                        reinterpret_cast<const char*>(association.dot11Ssid.ucSSID),
                        std::min<size_t>(association.dot11Ssid.uSSIDLength, 32));
                    result.signal_bars = static_cast<u8>(
                        std::min<ULONG>(3, (association.wlanSignalQuality + 32) / 33));
                    // Read only the associated SSID/BSSID from Windows' existing
                    // BSS cache. Never scan networks or read saved Wi-Fi profiles.
                    WLAN_BSS_LIST* list{};
                    auto ssid = association.dot11Ssid;
                    if (get_bss_list &&
                        get_bss_list(client, &guid, &ssid, association.dot11BssType,
                                     connection.wlanSecurityAttributes.bSecurityEnabled,
                                     nullptr, &list) == ERROR_SUCCESS && list) {
                        constexpr size_t header_size = offsetof(WLAN_BSS_LIST, wlanBssEntries);
                        const size_t capacity = list->dwTotalSize >= header_size
                            ? (list->dwTotalSize - header_size) / sizeof(WLAN_BSS_ENTRY) : 0;
                        for (size_t i = 0; i < std::min<size_t>(list->dwNumberOfItems, capacity); ++i) {
                            const auto& entry = list->wlanBssEntries[i];
                            if (!std::equal(result.access_point_mac.begin(),
                                            result.access_point_mac.end(), entry.dot11Bssid)) {
                                continue;
                            }
                            result.frequency_khz = entry.ulChCenterFrequency;
                            result.rssi_dbm = entry.lRssi;
                            const auto mhz = result.frequency_khz / 1000;
                            if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0) {
                                result.wireless_channel = static_cast<u16>((mhz - 2407) / 5);
                            } else if (mhz == 2484) {
                                result.wireless_channel = 14;
                            } else if (mhz >= 5000 && mhz < 5925 && mhz % 5 == 0) {
                                result.wireless_channel = static_cast<u16>((mhz - 5000) / 5);
                            } else if (mhz >= 5955 && mhz <= 7115 && (mhz - 5950) % 5 == 0) {
                                result.wireless_channel = static_cast<u16>((mhz - 5950) / 5);
                            }
                            break;
                        }
                        free_memory(list);
                    }
                    // Some drivers do not publish a BSS entry. Query the actual
                    // channel as a fallback; do not infer it from an SSID suffix.
                    if (!result.wireless_channel) {
                        DWORD channel_size{};
                        void* channel_data{};
                        if (query_interface(client, &guid, wlan_intf_opcode_channel_number,
                                            nullptr, &channel_size, &channel_data, &value_type)
                                == ERROR_SUCCESS && channel_data) {
                            if (channel_size >= sizeof(ULONG)) {
                                const auto channel = *static_cast<ULONG*>(channel_data);
                                if (channel <= 233) {
                                    result.wireless_channel = static_cast<u16>(channel);
                                }
                            }
                            free_memory(channel_data);
                        }
                    }
                }
            }
            free_memory(data);
        }
        close_handle(client, nullptr);
    }
    FreeLibrary(library);
}
} // namespace

std::vector<NetworkInterface> GetAvailableNetworkInterfaces() {
    std::vector<IP_ADAPTER_ADDRESSES> adapter_addresses;
    DWORD ret = ERROR_BUFFER_OVERFLOW;
    DWORD buf_size = 0;

    // retry up to 5 times
    for (int i = 0; i < 5 && ret == ERROR_BUFFER_OVERFLOW; i++) {
        ret = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS,
            nullptr, adapter_addresses.data(), &buf_size);

        if (ret != ERROR_BUFFER_OVERFLOW) {
            break;
        }

        adapter_addresses.resize((buf_size / sizeof(IP_ADAPTER_ADDRESSES)) + 1);
    }

    if (ret != NO_ERROR) {
        LOG_ERROR(Network, "Failed to get network interfaces with GetAdaptersAddresses");
        return {};
    }

    std::vector<NetworkInterface> result;

    for (auto current_address = adapter_addresses.data(); current_address != nullptr;
         current_address = current_address->Next) {
        if (current_address->FirstUnicastAddress == nullptr ||
            current_address->FirstUnicastAddress->Address.lpSockaddr == nullptr) {
            continue;
        }

        if (current_address->OperStatus != IfOperStatusUp ||
            current_address->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        const auto ip_addr = Common::BitCast<struct sockaddr_in>(
                                 *current_address->FirstUnicastAddress->Address.lpSockaddr)
                                 .sin_addr;

        ULONG mask = 0;
        if (ConvertLengthToIpv4Mask(current_address->FirstUnicastAddress->OnLinkPrefixLength,
                                    &mask) != NO_ERROR) {
            LOG_ERROR(Network, "Failed to convert IPv4 prefix length to subnet mask");
            continue;
        }

        struct in_addr gateway = {.S_un{.S_addr{0}}};
        if (current_address->FirstGatewayAddress != nullptr &&
            current_address->FirstGatewayAddress->Address.lpSockaddr != nullptr) {
            gateway = Common::BitCast<struct sockaddr_in>(
                          *current_address->FirstGatewayAddress->Address.lpSockaddr)
                          .sin_addr;
        }

        NetworkInterface iface{
            .name{Common::UTF16ToUTF8(std::wstring{current_address->FriendlyName})},
            .ip_address{ip_addr},
            .subnet_mask = in_addr{.S_un{.S_addr{mask}}},
            .gateway = gateway,
            .wireless = current_address->IfType == IF_TYPE_IEEE80211};
        std::copy_n(current_address->PhysicalAddress,
                    std::min<size_t>(current_address->PhysicalAddressLength, iface.mac_address.size()),
                    iface.mac_address.begin());
        size_t dns_index = 0;
        for (auto* dns = current_address->FirstDnsServerAddress;
             dns && dns_index < iface.dns_servers.size(); dns = dns->Next) {
            if (dns->Address.lpSockaddr && dns->Address.lpSockaddr->sa_family == AF_INET) {
                iface.dns_servers[dns_index++] =
                    reinterpret_cast<const sockaddr_in*>(dns->Address.lpSockaddr)->sin_addr;
            }
        }
        ReadWirelessConnection(*current_address, iface);
        result.push_back(std::move(iface));
    }

    return result;
}

#else

std::vector<NetworkInterface> GetAvailableNetworkInterfaces() {
    struct ifaddrs* ifaddr = nullptr;

    if (getifaddrs(&ifaddr) != 0) {
        LOG_ERROR(Network, "Failed to get network interfaces with getifaddrs: {}",
                  std::strerror(errno));
        return {};
    }

    std::vector<NetworkInterface> result;

    for (auto ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_netmask == nullptr) {
            continue;
        }

        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        u32 gateway{};

        std::ifstream file{"/proc/net/route"};
        if (!file.is_open()) {
            LOG_ERROR(Network, "Failed to open \"/proc/net/route\"");

            result.emplace_back(NetworkInterface{
                .name{ifa->ifa_name},
                .ip_address{Common::BitCast<struct sockaddr_in>(*ifa->ifa_addr).sin_addr},
                .subnet_mask{Common::BitCast<struct sockaddr_in>(*ifa->ifa_netmask).sin_addr},
                .gateway{in_addr{.s_addr = gateway}}});
            continue;
        }

        // ignore header
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        bool gateway_found = false;

        for (std::string line; std::getline(file, line);) {
            std::istringstream iss{line};

            std::string iface_name;
            iss >> iface_name;
            if (iface_name != ifa->ifa_name) {
                continue;
            }

            iss >> std::hex;

            u32 dest{};
            iss >> dest;
            if (dest != 0) {
                // not the default route
                continue;
            }

            iss >> gateway;

            u16 flags{};
            iss >> flags;

            // flag RTF_GATEWAY (defined in <linux/route.h>)
            if ((flags & 0x2) == 0) {
                continue;
            }

            gateway_found = true;
            break;
        }

        if (!gateway_found) {
            gateway = 0;
        }

        result.emplace_back(NetworkInterface{
            .name{ifa->ifa_name},
            .ip_address{Common::BitCast<struct sockaddr_in>(*ifa->ifa_addr).sin_addr},
            .subnet_mask{Common::BitCast<struct sockaddr_in>(*ifa->ifa_netmask).sin_addr},
            .gateway{in_addr{.s_addr = gateway}}});
    }

    freeifaddrs(ifaddr);

    return result;
}

#endif

std::optional<NetworkInterface> GetSelectedNetworkInterface() {
    // If airplane mode is enabled, return no interface (similar to Switch's airplane mode)
    if (Settings::values.airplane_mode.GetValue()) {
        return std::nullopt;
    }

    const auto& selected_network_interface = Settings::values.network_interface.GetValue();
    const auto network_interfaces = Network::GetAvailableNetworkInterfaces();
    if (network_interfaces.empty()) {
        LOG_ERROR(Network, "GetAvailableNetworkInterfaces returned no interfaces");
        return std::nullopt;
    }

    const auto res =
        std::ranges::find_if(network_interfaces, [&selected_network_interface](const auto& iface) {
            return iface.name == selected_network_interface;
        });

    if (res == network_interfaces.end()) {
        // Prefer an adapter with a gateway; a VPN tunnel often sorts first but peers can't reach it.
        const auto with_gateway = std::ranges::find_if(network_interfaces, [](const auto& iface) {
            return iface.gateway.s_addr != 0;
        });
        const auto& chosen =
            with_gateway != network_interfaces.end() ? *with_gateway : network_interfaces.front();
        static bool print_error = true;
        if (print_error) {
            LOG_INFO(Network, "Selected interface \"{}\" not explicitly set or found; using primary interface \"{}\"",
                      selected_network_interface, chosen.name);
            print_error = false;
        }

        return chosen;
    }

    return *res;
}

void SelectFirstNetworkInterface() {
    const auto network_interfaces = Network::GetAvailableNetworkInterfaces();

    if (network_interfaces.empty()) {
        return;
    }

    Settings::values.network_interface.SetValue(network_interfaces[0].name);
}

} // namespace Network
