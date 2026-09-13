#include "ring_recon.h"
#include <vector>
#include <iostream>
struct ServiceFragment {
    struct { bool shiftWL2=true; } m_ringConfig;
    int m_ringBlockIndex=0;
    std::vector<float> m_ringPrevWL2[8];
    float m_ringPrevAngle[8]={},m_ringPrevRadius[8]={};
    std::vector<std::vector<float>> step(std::vector<float> input) {
        std::vector<std::vector<float>> lists[2];
        std::vector<float> angs[2],rads[2],secs[2];
        for(float value:input) { lists[1].push_back({value}); angs[1].push_back(value); rads[1].push_back(1); secs[1].push_back(0); }
        int c=0; double chSectorStartDeg=0;
        // SERVICE_FRAGMENT
        ++m_ringBlockIndex;
        return lists[1];
    }
};
int main() {
    ServiceFragment service;
    std::vector<double> previous;
    bool thirdMismatch=false;
    for(int block=0;block<3;++block) {
        float a=float(2*block+1),b=a+1;
        auto result=service.step({a,b});
        std::vector<double> raw={0,a,0,b},wl1,wl2,last;
        ringrecon::splitBlock(raw,1,2,block>0,previous,{},wl1,wl2,last);
        previous=last;
        std::cout<<"block="<<block+1<<" service="<<result[0][0]<<','<<result[1][0]<<" cpu_reference="<<wl2[0]<<','<<wl2[1]<<'\n';
        if(block==2) thirdMismatch=result[0][0]==3 && wl2[0]==4;
    }
    std::cout<<"wl2_original_tail_lost: "<<(thirdMismatch?"REPRODUCED":"NOT_REPRODUCED")<<'\n';
    return thirdMismatch?0:1;
}
