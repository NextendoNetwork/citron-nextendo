// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string_view>

namespace Common::NextendoFriends {
// Native FriendCode stores only dddd-dddd-dddd; MyPage supplies the SW- label.
inline std::array<char, 15> NativeFriendCode(std::string_view code) {
    if (code.starts_with("SW-")) code.remove_prefix(3);
    std::array<char, 12> digits{};
    if (code.size() != 12 && code.size() != 14) return {};
    size_t written = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        if (code.size() == 14 && (i == 4 || i == 9)) {
            if (code[i] != '-') return {};
            continue;
        }
        if (code[i] < '0' || code[i] > '9' || written == digits.size()) return {};
        digits[written++] = code[i];
    }
    if (written != digits.size()) return {};
    std::array<char, 15> result{};
    for (size_t i = 0; i < digits.size(); ++i) result[i + i / 4] = digits[i];
    result[4] = result[9] = '-';
    return result;
}
} // namespace Common::NextendoFriends
