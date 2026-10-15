// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in UCH account-bound UDP bootstrap. No payload/credential logging.
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Common::UchAccountTransport {
inline bool Enabled(u64 title) {
    const char* value = std::getenv("NEXTENDO_UCH_ACCOUNT_TRANSPORT");
    return title == 0x0100FCF002A58000ULL && value && std::string_view(value) == "1";
}
inline s64 Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
struct State {
    std::mutex mutex;
    std::array<u8,36> frame{};
    std::array<u8,4> ip{};
    u16 port{};
    s64 expires{};
};
inline State state;
inline void Wipe(std::vector<u8>& buffer) {
    std::fill(buffer.begin(),buffer.end(),0);
    buffer.clear();
}
inline void Observe(std::span<const u8> bytes, std::vector<u8>& pending) {
    constexpr size_t limit = 131072;
    if (pending.size()+bytes.size()>limit) { Wipe(pending); return; }
    pending.insert(pending.end(),bytes.begin(),bytes.end());
    std::string_view raw(reinterpret_cast<const char*>(pending.data()),pending.size());
    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end==std::string_view::npos) { if (pending.size()>16384) Wipe(pending); return; }
    if (header_end>16384) { Wipe(pending); return; }
    std::string headers(raw.substr(0,header_end));
    std::transform(headers.begin(),headers.end(),headers.begin(),[](unsigned char c){ return c>='A'&&c<='Z'?c+32:c; });
    const size_t marker=headers.find("\r\ncontent-length:");
    if (marker==std::string::npos) { Wipe(pending); return; }
    size_t start=marker+17;
    while (start<headers.size() && headers[start]==' ') start++;
    size_t length{};
    auto parsed=std::from_chars(headers.data()+start,headers.data()+headers.size(),length);
    if (parsed.ec!=std::errc{} || length>limit-header_end-4) { Wipe(pending); return; }
    if (pending.size()<header_end+4+length) return;
    if (headers.starts_with("http/1.1 200 ") || headers.starts_with("http/1.0 200 ")) {
        auto root=nlohmann::json::parse(pending.begin()+header_end+4,pending.begin()+header_end+4+length,nullptr,false);
        if (!root.is_discarded() && root.is_object() && root.contains("nextendoTransport")) {
            try {
                const auto& extension=root.at("nextendoTransport");
                const auto ticket=extension.at("ticket").get<std::string>();
                const auto address=extension.at("relayIP").get<std::string>();
                const auto expires=extension.at("expiresMs").get<s64>();
                const auto port=extension.at("relayPort").get<int>();
                const auto now=Now();
                if (extension.at("protocol")!="NXU1" || ticket.size()!=43 || expires<=now || expires>now+35000 || port<1 || port>65535) { Wipe(pending); return; }
                std::array<u8,36> frame{'N','X','U','1'};
                size_t at=4; u32 accumulator=0; unsigned bits=0;
                for (char c:ticket) {
                    int value=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='-'?62:c=='_'?63:-1;
                    if (value<0) { Wipe(pending); return; }
                    accumulator=(accumulator<<6)|static_cast<unsigned>(value); bits+=6;
                    if (bits>=8) { bits-=8; if(at>=frame.size()) { Wipe(pending); return; } frame[at++]=static_cast<u8>(accumulator>>bits); }
                }
                if(at!=36 || (accumulator&3)!=0) { Wipe(pending); return; }
                std::array<u8,4> ip{}; size_t pos=0;
                for (size_t i=0;i<4;i++) {
                    const size_t end=i==3?address.size():address.find('.',pos);
                    if(end==std::string::npos || end==pos) { Wipe(pending); return; }
                    unsigned value{}; auto number=std::from_chars(address.data()+pos,address.data()+end,value);
                    if(number.ec!=std::errc{} || number.ptr!=address.data()+end || value>255) { Wipe(pending); return; }
                    ip[i]=static_cast<u8>(value); pos=end+1;
                }
                if(ip==std::array<u8,4>{}) { Wipe(pending); return; }
                std::lock_guard lock(state.mutex);
                state.frame=frame; state.ip=ip; state.port=static_cast<u16>(port); state.expires=expires;
                std::fill(frame.begin(),frame.end(),0);
            } catch (const nlohmann::json::exception&) { }
        }
    }
    Wipe(pending); // The Go control plane closes after each length-framed response.
}
inline bool IsConnect(std::span<const u8> packet) {
    if(packet.size()!=19 || packet[0]!=0 || packet[1]!=0 || packet[2]!=1) return false;
    auto read=[&](size_t at){ return (u32(packet[at])<<24)|(u32(packet[at+1])<<16)|(u32(packet[at+2])<<8)|packet[at+3]; };
    return read(11)==16777984 && read(15)==2944057210;
}
inline std::optional<std::array<u8,36>> Bootstrap(std::array<u8,4> ip,u16 port) {
    std::lock_guard lock(state.mutex);
    if(state.expires<=Now() || state.ip!=ip || state.port!=port) return std::nullopt;
    return state.frame;
}
}
