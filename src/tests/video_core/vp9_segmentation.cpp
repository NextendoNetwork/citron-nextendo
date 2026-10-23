// Offline fixtures. Compile against the same header used by the client.
#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <vector>
#include "video_core/host1x/codecs/vp9_segmentation.h"

struct Segment {
    unsigned char enabled{}, update_map{}, temporal_update{}, abs_delta{};
    std::array<unsigned,8> feature_mask{};
    std::array<std::array<short,4>,8> feature_data{};
};
struct Bits {
    std::vector<bool> values;
    void WriteBit(bool value){ values.push_back(value); }
    void WriteU(unsigned value,unsigned count){while(count)WriteBit((value >> --count)&1);}
    void WriteS(int value,unsigned count){WriteU(value < 0 ? -value : value,count);WriteBit(value<0);}
};
struct Reader {
    const std::vector<bool>& values;
    unsigned pos{};
    unsigned bit(){REQUIRE(pos<values.size());return values[pos++];}
    unsigned number(unsigned count){unsigned value=0;while(count--)value=(value<<1)|bit();return value;}
    // Parse the emitted header using the VP9 decoder syntax, independently
    // from the encoder's iteration and WriteS implementation.
    void Check(const Segment& source,const std::array<std::uint8_t,7>& tree,
               const std::array<std::uint8_t,3>& prediction){
        REQUIRE(bit()==bool(source.enabled));
        if(source.enabled){
            REQUIRE(bit()==bool(source.update_map));
            if(source.update_map){
                for(unsigned i=0;i<7;i++)REQUIRE((bit()?number(8):255)==tree[i]);
                REQUIRE(bit()==bool(source.temporal_update));
                if(source.temporal_update)for(unsigned i=0;i<3;i++)REQUIRE((bit()?number(8):255)==prediction[i]);
            }
            REQUIRE(bit()==1); // Full hardware state supplied every frame.
            REQUIRE(bit()==bool(source.abs_delta));
            for(unsigned i=0;i<8;i++)for(unsigned j=0;j<4;j++){
                bool enabled=bit();REQUIRE(enabled==bool(source.feature_mask[i]&(1<<j)));
                if(enabled){
                    unsigned count=j==0?8:j==1?6:j==2?2:0;
                    int value=number(count);
                    if(j<2 && bit())value=-value;
                    REQUIRE(value==source.feature_data[i][j]);
                }
            }
        }
        REQUIRE(pos==values.size());
    }
};
TEST_CASE("VP9 segmentation feature states round trip", "[video][vp9]"){
    unsigned cases=0;
    const std::array<std::uint8_t,7> tree{255,1,2,128,200,254,255};
    const std::array<std::uint8_t,3> pred{17,255,240};
    for(unsigned enabled=0;enabled<2;enabled++)for(unsigned update=0;update<2;update++)
    for(unsigned temporal=0;temporal<2;temporal++)for(unsigned absolute=0;absolute<2;absolute++)
    for(unsigned mode=0;mode<3;mode++){
        Segment input{};input.enabled=enabled;input.update_map=update;
        input.temporal_update=temporal;input.abs_delta=absolute;
        for(unsigned i=0;i<8;i++){
            input.feature_mask[i]=mode==0?0:mode==1?15:(1u<<(i%4));
            for(unsigned j=0;j<4;j++){
                int value=j==0?255:j==1?63:j==2?3:0;
                if(j<2 && (i&1))value=-value;
                input.feature_data[i][j]=value;
            }
        }
        Bits bits;Tegra::Decoder::WriteVp9Segmentation(bits,input,tree,pred);
        Reader{bits.values}.Check(input,tree,pred);++cases;
    }
    std::cout<<cases<<" VP9 segmentation bitstream round trips passed\n";
}
