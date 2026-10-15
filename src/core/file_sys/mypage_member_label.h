// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include "common/common_types.h"

namespace FileSys::MyPageMemberLabel {
// Same-size edit: retain MSBT offsets, SARC offsets and all layout/theme assets.
inline bool Replace(std::vector<u8>& bytes) {
    constexpr std::string_view signature = "MsgStdBn";
    constexpr std::string_view key = "FrdDetail_Frd_HeaderDate";
    constexpr std::u16string_view label = u"Miembro desde (Nextendo)";
    bool changed = false;
    for (size_t start = 0; start + 32 <= bytes.size(); ++start) {
        if (!std::equal(signature.begin(), signature.end(), bytes.begin() + start)) continue;
        const bool little = bytes[start + 8] == 0xff && bytes[start + 9] == 0xfe;
        if (!little || bytes[start + 12] != 1) continue;
        auto read32 = [&](size_t p) -> u32 {
            return u32(bytes[p]) | u32(bytes[p+1]) << 8 | u32(bytes[p+2]) << 16 | u32(bytes[p+3]) << 24;
        };
        const size_t size = read32(start + 18);
        if (size < 32 || size > bytes.size() - start) continue;
        const size_t end = start + size;
        std::optional<u32> index;
        size_t txt = 0, txt_size = 0;
        for (size_t section = start + 32; section + 16 <= end;) {
            const size_t payload = section + 16;
            const size_t length = read32(section + 4);
            if (length > end - payload) break;
            if (std::equal(bytes.begin()+section, bytes.begin()+section+4, "LBL1") && length >= 4) {
                const size_t groups = read32(payload);
                if (groups <= (length - 4) / 8) {
                    for (size_t g = 0; g < groups; ++g) {
                        const size_t count = read32(payload + 4 + g * 8);
                        const size_t offset = read32(payload + 8 + g * 8);
                        if (offset >= length || count > length / 5) continue;
                        size_t p = payload + offset;
                        for (size_t n = 0; n < count && p < payload + length; ++n) {
                            const size_t len = bytes[p++];
                            if (len + 4 > payload + length - p) break;
                            if (len == key.size() && std::equal(key.begin(), key.end(), bytes.begin()+p))
                                index = read32(p + len);
                            p += len + 4;
                        }
                    }
                }
            } else if (std::equal(bytes.begin()+section, bytes.begin()+section+4, "TXT2")) {
                txt = payload; txt_size = length;
            }
            section = start + ((payload + length - start + 15) & ~size_t(15));
        }
        if (!index || txt_size < 4) continue;
        const size_t count = read32(txt);
        if (count > (txt_size - 4) / 4 || *index >= count) continue;
        const size_t begin = read32(txt + 4 + *index * 4);
        const size_t finish = *index + 1 < count ? read32(txt + 8 + *index * 4) : txt_size;
        if (begin < 4 + count * 4 || finish > txt_size || finish < begin ||
            (finish - begin) % 2 || (label.size() + 1) * 2 > finish - begin) continue;
        std::fill(bytes.begin()+txt+begin, bytes.begin()+txt+finish, 0);
        for (size_t i = 0; i < label.size(); ++i) {
            bytes[txt+begin+i*2] = static_cast<u8>(label[i]);
            bytes[txt+begin+i*2+1] = static_cast<u8>(label[i] >> 8);
        }
        changed = true;
        start = end - 1;
    }
    return changed;
}

inline std::optional<std::vector<u8>> Patch(std::span<const u8> input) {
    constexpr size_t limit = 8 * 1024 * 1024;
    if (input.size() > limit) return std::nullopt;
    const bool yaz = input.size() >= 16 && std::equal(input.begin(), input.begin()+4, "Yaz0");
    std::vector<u8> data;
    if (!yaz) data.assign(input.begin(), input.end());
    else {
        const size_t size = u32(input[4]) << 24 | u32(input[5]) << 16 | u32(input[6]) << 8 | input[7];
        if (size > limit) return std::nullopt;
        data.reserve(size);
        size_t p = 16;
        while (data.size() < size) {
            if (p >= input.size()) return std::nullopt;
            const u8 code = input[p++];
            for (int bit = 7; bit >= 0 && data.size() < size; --bit) {
                if (p >= input.size()) return std::nullopt;
                if (code & (1 << bit)) data.push_back(input[p++]);
                else {
                    if (input.size() - p < 2) return std::nullopt;
                    const u8 a = input[p++], b = input[p++];
                    const size_t distance = ((a & 15) << 8 | b) + 1;
                    size_t length = a >> 4;
                    if (!length) {
                        if (p >= input.size()) return std::nullopt;
                        length = input[p++] + 18;
                    } else length += 2;
                    if (distance > data.size() || length > size - data.size()) return std::nullopt;
                    while (length--) data.push_back(data[data.size() - distance]);
                }
            }
        }
    }
    if (!Replace(data)) return std::nullopt;
    if (!yaz) return data;
    std::vector<u8> result(16);
    std::copy_n(input.begin(), 16, result.begin());
    for (size_t p = 0; p < data.size(); p += 8) {
        result.push_back(0xff);
        const size_t count = std::min<size_t>(8, data.size() - p);
        result.insert(result.end(), data.begin()+p, data.begin()+p+count);
    }
    return result;
}
} // namespace FileSys::MyPageMemberLabel
