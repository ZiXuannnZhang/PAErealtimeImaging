#include "PacketAssemblyBuffer.h"
#include "RingBlockAssembler.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <cstdint>
#include <cmath>
uint16_t float32ToFloat16(float value); // extracted unchanged from audited FileSaver.cpp

DataPacket packet(uint16_t seq, int16_t value) {
    DataPacket p; p.triggerSeq=7; p.packetSeq=seq; p.dataSize=1440;
    for(int i=0;i<360;++i) {
        std::memcpy(p.data+i*4,&value,2);
        std::memcpy(p.data+i*4+2,&value,2);
    }
    return p;
}
int main() {
    int reproduced=0;
    AcqConfig cfg; cfg.bitsPerChannel=16; cfg.sampleIntervalNs=4; cfg.acqTimeNs=2880;
    PacketAssemblyBuffer assembly;
    assembly.insertPacket(packet(101,20));
    assembly.insertPacket(packet(100,10));
    TriggerGroup g; assembly.exportTo(g,cfg);
    bool reorder=assembly.receivedCount()==1 && g.freqA[0]==20 && g.freqA[360]==0;
    reproduced+=reorder;
    std::cout<<"packet_first_arrival_anchor: "<<(reorder?"REPRODUCED":"NOT_REPRODUCED")<<" count="<<assembly.receivedCount()<<" first_sample="<<g.freqA[0]<<" second_packet_sample="<<g.freqA[360]<<'\n';
    assembly.reset(); assembly.insertPacket(packet(100,10)); assembly.insertPacket(packet(102,30));
    assembly.exportTo(g,cfg);
    bool falseComplete=assembly.isComplete(2) && g.isComplete && g.freqA[360]==0;
    reproduced+=falseComplete;
    std::cout<<"packet_count_without_expected_slots: "<<(falseComplete?"REPRODUCED":"NOT_REPRODUCED")<<" complete="<<g.isComplete<<" missing_slot_sample="<<g.freqA[360]<<'\n';
    RingBlockAssembler ring; int channels[8]={1,1,0,0,0,0,0,0};
    std::vector<float> captured;
    ring.setBlockCallback([&](std::vector<float>&& raw,std::vector<float>&&,std::vector<uint8_t>&&,int){captured=std::move(raw);});
    ring.configure(channels,2,1,0,180,1,32,1,0);
    float t10=10,t11=11;
    ring.pushChannelLine(0,10,&t10,1);
    ring.pushChannelLine(0,11,&t11,1);
    ring.pushChannelLine(1,11,&t11,1);
    ring.pushChannelLine(1,10,&t10,1);
    bool ringOrder=captured==std::vector<float>({11,11,10,10});
    reproduced+=ringOrder;
    std::cout<<"ring_completed_arrival_order: "<<(ringOrder?"REPRODUCED":"NOT_REPRODUCED")<<" block=";
    for(float x:captured) std::cout<<x<<',';
    std::cout<<'\n';
    RingBlockAssembler gaps; int one[8]={1,0,0,0,0,0,0,0};
    std::vector<float> gapAngles;
    gaps.setBlockCallback([&](std::vector<float>&&,std::vector<float>&& ang,std::vector<uint8_t>&&,int){gapAngles=std::move(ang);});
    gaps.configure(one,3,1,0,360,1,32,1,0);
    float sample=1;
    for(uint16_t seq : {0,2,3}) gaps.pushChannelLine(0,seq,&sample,1);
    bool gap=gapAngles==std::vector<float>({0,1,1}); reproduced+=gap;
    std::cout<<"ring_trigger_gap_angle: "<<(gap?"REPRODUCED":"NOT_REPRODUCED")<<" angles=";
    for(float x:gapAngles) std::cout<<x<<',';
    std::cout<<" expected=0,1,2 (wire triggers 0,2,3)\n";
    float subnormal=std::ldexp(512.75f,-24);
    auto half=float32ToFloat16(subnormal);
    bool rounding=half==0x200; reproduced+=rounding;
    std::cout<<"half_subnormal_rounding: "<<(rounding?"REPRODUCED":"NOT_REPRODUCED")<<" actual=0x"<<std::hex<<half<<" expected=0x201"<<std::dec<<'\n';
    return reproduced==5 ? 0 : 1;
}
