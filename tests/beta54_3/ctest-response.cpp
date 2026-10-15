// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <iostream>
#include "core/hle/service/ssl/ctest_response.h"
using Service::SSL::NormalizeCtestIpResponse;
int main(){
 auto packet=[](std::string body){return "HTTP/1.1 200 OK\r\nContent-Length: "+std::to_string(body.size())+"\r\nContent-Type: application/json\r\n\r\n"+body;};
 unsigned passed=0;
 auto check=[&](bool okay){if(!okay)std::exit(1);++passed;};
 const auto before=packet("{\"ip\":\"192.0.2.1\"}");
 auto after=NormalizeCtestIpResponse(before);check(after.has_value());
 check(after->find("\"global_ip\":\"192.0.2.1\"")!=std::string::npos);
 const auto split=after->find("\r\n\r\n");auto body=after->substr(split+4);
 check(after->find("Content-Length: "+std::to_string(body.size())+"\r\n")!=std::string::npos);
 check(!NormalizeCtestIpResponse(packet("{\"global_ip\":\"192.0.2.1\"}")));
 check(!NormalizeCtestIpResponse(before.substr(0,before.size()-1)));
 check(!NormalizeCtestIpResponse(packet("{\"ip\":null}")));
 check(!NormalizeCtestIpResponse(packet("{\"error\":\"unavailable\"}")));
 check(!NormalizeCtestIpResponse("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"));
 std::cout<<"Protocol checks passed: "<<passed<<'\n';
}
