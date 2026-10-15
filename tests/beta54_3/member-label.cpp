// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstring>
#include <iostream>
#include "core/file_sys/mypage_member_label.h"
using namespace FileSys::MyPageMemberLabel;
void put(std::vector<u8>& b,size_t p,u32 v){for(int i=0;i<4;++i)b[p+i]=v>>(i*8);}
std::vector<u8> sample(){
 std::vector<u8>b(0x120);std::memcpy(b.data(),"MsgStdBn",8);b[8]=0xff;b[9]=0xfe;b[12]=1;b[14]=2;put(b,18,b.size());
 std::memcpy(b.data()+0x20,"LBL1",4);std::string key="FrdDetail_Frd_HeaderDate";put(b,0x24,12+1+key.size()+4);put(b,0x30,1);put(b,0x34,1);put(b,0x38,12);b[0x3c]=key.size();std::memcpy(b.data()+0x3d,key.data(),key.size());put(b,0x3d+key.size(),0);
 std::memcpy(b.data()+0x60,"TXT2",4);put(b,0x64,0x80);put(b,0x70,2);put(b,0x74,12);put(b,0x78,0x60);
 auto text=u"When You Became Friends:";std::memcpy(b.data()+0x7c,text,48);std::memcpy(b.data()+0xd0,u"Other text",20);return b;
}
int main(){auto b=sample();auto patched=Patch(b);assert(patched);assert(patched->size()==b.size());assert(std::memcmp(patched->data()+0x7c,u"Miembro desde (Nextendo)",48)==0);assert(std::equal(b.begin()+0xd0,b.end(),patched->begin()+0xd0));
 auto no=b;no[0x3d]='X';assert(!Patch(no));auto bad=b;put(bad,0x24,0xffffffff);assert(!Patch(bad));bad=b;put(bad,0x34,0xffffffff);assert(!Patch(bad));bad=b;put(bad,0x74,0xffffffff);assert(!Patch(bad));bad=b;bad[12]=0;assert(!Patch(bad));
 for(size_t n=0;n<b.size();++n)assert(!Patch(std::span(b).first(n)));
 std::vector<u8> yaz(16);std::memcpy(yaz.data(),"Yaz0",4);auto size=b.size();for(int i=0;i<4;++i)yaz[4+i]=size>>((3-i)*8);for(size_t i=0;i<b.size();i+=8){yaz.push_back(0xff);yaz.insert(yaz.end(),b.begin()+i,b.begin()+std::min(i+8,b.size()));}auto yp=Patch(yaz);assert(yp);assert(Patch(*yp));
 std::vector<u8> malformed(19);std::memcpy(malformed.data(),"Yaz0",4);malformed[7]=10;assert(!Patch(malformed));
 std::cout<<"PASS: label, other text preserved, malformed bounds, every truncation, Yaz0 round trip\n";
}
