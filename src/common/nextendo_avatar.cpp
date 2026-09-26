// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <mutex>

#include "common/nextendo_avatar.h"

namespace Common::NextendoAvatar {

namespace {

std::mutex g_mutex;
std::string g_self_jpeg_base64;

} // Anonymous namespace

std::vector<u8> DecodeBase64(std::string_view text) {
    std::array<int, 256> table;
    table.fill(-1);
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    }

    std::vector<u8> out;
    out.reserve(text.size() / 4 * 3);

    int val = 0;
    int bits = -8;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        const int d = table[static_cast<unsigned char>(c)];
        if (d == -1) {
            continue;
        }
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<u8>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

void SetSelfJPEGBase64(std::string_view base64_jpeg) {
    std::lock_guard lock{g_mutex};
    g_self_jpeg_base64 = base64_jpeg;
}

std::vector<u8> GetSelfJPEG() {
    std::lock_guard lock{g_mutex};
    if (g_self_jpeg_base64.empty()) {
        return {};
    }
    return DecodeBase64(g_self_jpeg_base64);
}

} // namespace Common::NextendoAvatar
