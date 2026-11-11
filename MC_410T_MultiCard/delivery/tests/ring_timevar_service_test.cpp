#include "ImagingSvc.h"
#include "ImagingSharedMemory.h"
#include "RingTimevarLowpassSettings.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
void need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
struct RingTimevarServiceTest {
    static void run() {
        ImagingSvc svc;
        svc.m_ringConfig.sampDepth=987;
        svc.m_ringSysDelayCh[0][0]=123;
        QJsonArray channels;for(int i=0;i<8;++i)channels.append(i==0?1:0);
        QJsonObject ring{{"sampDepth",2030},{"daqHz",250e6},{"alinesPerChannelPerBlock",4},
            {"enabledChannels",channels},{"sysDelay",QJsonArray{10,12}}};
        for(const auto& invalid:{QJsonValue(QJsonValue::Null),QJsonValue(1),
            QJsonValue(QJsonObject{{"enabled",true},{"p2",2031}}),
            QJsonValue(QJsonObject{{"windowLength",1}}),
            QJsonValue(QJsonObject{{"transition","bogus"}})}) {
            ring["timevarLowpass"]=invalid;svc.processRingConfigure(ring);
            need(svc.m_ringConfig.sampDepth==987&&svc.m_ringSysDelayCh[0][0]==123,"rejected config mutated live state");
            need(!svc.m_ringTimevarConfig.enabled,"invalid config partly enabled");
        }
        const int n=2030,lines=4,nx=20,frame=nx*nx;
        ring_input::NativeMapping shm; ring_input::Mutex inputMutex;
        need(shm.create(static_cast<size_t>(ringImagingShmTotalSizeV3(n*lines,frame,lines,frame)))==ring_input::Result::Ok,"isolated SHM creation failed (other Ring instance running?)");
        std::memset(shm.data(),0,shm.capacity());
        auto* h=static_cast<RingImagingShmHeader*>(shm.data());
        h->magic=0x52494E47;h->version=2;h->block_size=n*lines;h->alines=lines;
        h->frame_size=frame;h->nx=nx;h->ny=nx;h->display_nx=nx;h->display_ny=nx;
        h->display_frame_size=frame;h->snapshot_step=1;
        need(ring_input::initialize(inputMutex,shm.data(),shm.capacity(),1,20)==ring_input::Result::Ok,"v3 initialize");
        ring["timevarLowpass"]=QJsonObject{{"enabled",true}};
        ring["fov"]=.002;ring["gridSize"]=.0001;ring["alinesPerFrame"]=8;
        ring["dbrSigRemove"]=0;ring["delayCut"]=0;ring["maskLength"]=0;
        ring["shiftWL2"]=1;
        svc.processRingConfigure(ring);
        need(svc.m_ringCuda[0]&&svc.m_ringCuda[1]&&svc.m_ringTimevarConfig.enabled,"valid config failed");
        svc.m_running=true;
        auto publish = [&](uint64_t id, uint32_t seq, uint64_t generation, bool final, int direction) {
            auto* base=static_cast<uint8_t*>(shm.data())+sizeof(RingImagingShmHeader);
            std::vector<float> rawCopy(reinterpret_cast<float*>(base),reinterpret_cast<float*>(base)+h->block_size);
            auto* anglePtr=reinterpret_cast<float*>(base+ringAnglesOffset(int(h->block_size)));
            std::vector<float> angleCopy(anglePtr,anglePtr+h->alines);
            auto* channelPtr=base+ringChannelsOffset(int(h->block_size),int(h->alines));
            std::vector<uint8_t> channelCopy(channelPtr,channelPtr+h->alines);
            ring_input::Identity identity{1,id,1,generation,0,seq,direction,final,seq};
            need(ring_input::publish(inputMutex,shm.data(),shm.capacity(),identity,rawCopy.data(),angleCopy.data(),channelCopy.data(),20)==ring_input::Result::Ok,"v3 publish");
        };
        auto fill=[&](int seq,bool bad) {
            h=static_cast<RingImagingShmHeader*>(shm.data());
            h->block_seq=seq;h->block_ready=1;
            auto* raw=reinterpret_cast<float*>(h+1);
            for(int i=0;i<n*lines;++i)raw[i]=static_cast<float>((i%37)*.01);
            if(bad)raw[n*lines-1]=std::numeric_limits<float>::quiet_NaN();
            auto* angles=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(h+1)+ringAnglesOffset(n*lines));
            for(int i=0;i<lines;++i)angles[i]=(i/2)*180;
            auto* ch=reinterpret_cast<uint8_t*>(h+1)+ringChannelsOffset(n*lines,lines);
            for(int i=0;i<lines;++i)ch[i]=0;

        };
        fill(0,true);
        publish(1,0,0,false,-1); svc.processRingPulse(1,1);
        need(svc.m_ringBlockIndex==0&&svc.m_ringPrevWL2[0].empty(),"failed line submitted partial block/WL2 state");
        need(h->frame_seq==0,"failure generated snapshot");
        fill(1,false);
        std::vector<float> original(n*lines);
        std::memcpy(original.data(),h+1,original.size()*4);
        publish(2,0,1,false,-1); svc.processRingPulse(1,2);
        if(svc.m_ringBlockIndex!=1||svc.m_ringPrevWL2[0].empty())
            std::cerr<<"index="<<svc.m_ringBlockIndex<<" prev="<<svc.m_ringPrevWL2[0].size()
                     <<" CUDA="<<ring_recon_cuda_last_error()<<'\n';
        need(svc.m_ringBlockIndex==1&&!svc.m_ringPrevWL2[0].empty(),"valid block not processed");
        need(std::memcmp(original.data(),h+1,original.size()*4)==0,"shared input modified in-place");
        const auto frameSeq=h->frame_seq;
        fill(2,false);publish(3,0,2,false,-1);svc.processRingPulse(1,3);
        need(svc.m_ringBlockIndex==1,"new generation did not reset");
        fill(3,false);publish(4,0,1,false,-1);svc.processRingPulse(1,4);
        need(svc.m_ringBlockIndex==1,"stale generation reached reconstruction");
        need(h->frame_seq==frameSeq+1,"stale produced snapshot");
        svc.resetRingRecon();need(svc.m_ringBlockIndex==0&&svc.m_ringPrevWL2[0].empty(),"timeout/reset retained signal state");
    }
};
int main(int argc,char** argv) {QCoreApplication app(argc,argv);try {
    RingTimevarServiceTest::run();std::cout<<"PASS service config atomicity, failure whole-block barrier, raw isolation, round/stale/reset\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
