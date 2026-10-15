// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/nextendo_friend_code.h"
#include <cassert>
#include <cstring>
#include <iostream>
int main(){
 using Common::NextendoFriends::NativeFriendCode;
 for(auto code:{"SW-0001-2345-6789","0001-2345-6789","000123456789"})
  assert(std::strcmp(NativeFriendCode(code).data(),"0001-2345-6789")==0);
 for(auto code:{"","SW-","0001-2345-678","0001-2345-67890","0001_2345_6789","0001-ABCD-6789","SW-SW-0001-2345-6789"})
  assert(NativeFriendCode(code)[0]=='\0');
 std::cout<<"PASS: native friend code formats, leading zeros, termination and malformed input\n";
}
