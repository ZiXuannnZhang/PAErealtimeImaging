#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "PaimageAcquisition/SocketReceiver.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace paimage;
constexpr int kCards=4;
constexpr int kOverflowPackets=9000;
constexpr std::uint16_t kDataPort=29101;
constexpr std::uint64_t kOverflowSession=7100;
constexpr std::uint64_t kRecoverySession=7101;

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    int overflow=0,failedDiscard=0,released=0,cardFrames=0,syncFrames=0,recoveryFrames=0,recoverySync=0;
};

void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}

template<class Predicate>
void waitFor(State& state,const std::string& label,Predicate predicate){
    std::unique_lock<std::mutex> lock(state.mutex);
    if(!state.changed.wait_for(lock,std::chrono::seconds(10),predicate))
        throw std::runtime_error("timeout: "+label+" overflow="+std::to_string(state.overflow)+
                                 " failedDiscard="+std::to_string(state.failedDiscard)+
                                 " released="+std::to_string(state.released)+
                                 " cardFrames="+std::to_string(state.cardFrames)+
                                 " recoveryFrames="+std::to_string(state.recoveryFrames)+
                                 " recoverySync="+std::to_string(state.recoverySync));
}

int sendPacket(SOCKET sender,std::uint16_t port,std::uint16_t trigger,std::uint16_t packet,std::size_t bytes){
    std::vector<std::uint8_t> payload(bytes,0);
    payload[0]=std::uint8_t(packet&255);payload[1]=std::uint8_t(packet>>8);
    payload[2]=std::uint8_t(trigger&255);payload[3]=std::uint8_t(trigger>>8);
    sockaddr_in destination{};destination.sin_family=AF_INET;destination.sin_port=htons(port);
    require(inet_pton(AF_INET,"127.0.0.1",&destination.sin_addr)==1,"destination conversion failed");
    return sendto(sender,reinterpret_cast<const char*>(payload.data()),int(payload.size()),0,
                  reinterpret_cast<sockaddr*>(&destination),sizeof(destination));
}
}

int main(){
    WSADATA wsa{};SOCKET sender=INVALID_SOCKET;std::unique_ptr<SocketReceiver> receiver;bool wsaStarted=false;
    try{
        require(WSAStartup(MAKEWORD(2,2),&wsa)==0,"WSAStartup failed");wsaStarted=true;
        State state;std::vector<SocketReceiver::Endpoint> endpoints;
        for(int card=0;card<kCards;++card)endpoints.push_back({std::uint16_t(kDataPort+card),"127.0.0.1"});
        std::vector<std::string> targets(kCards,"127.0.0.1");
        receiver=std::make_unique<SocketReceiver>(
            Config{kCards,180,32,0},endpoints,SocketReceiver::Endpoint{0,{}},targets,
            nullptr,nullptr,nullptr,
            [&state](Frame frame){
                if(!frame)return;std::lock_guard<std::mutex> lock(state.mutex);
                if(frame->measurementSession==kOverflowSession)++state.cardFrames;
                if(frame->measurementSession==kRecoverySession)++state.recoveryFrames;
                state.changed.notify_all();
            },
            [&state](std::uint16_t,const std::vector<Frame>& frames,bool){
                std::lock_guard<std::mutex> lock(state.mutex);
                for(const auto& frame:frames)if(frame&&frame->measurementSession==kOverflowSession)++state.syncFrames;
                for(const auto& frame:frames)if(frame&&frame->measurementSession==kRecoverySession){++state.recoverySync;break;}
                state.changed.notify_all();
            });
        receiver->observationSink=[&state](const Observation& observation){
            std::lock_guard<std::mutex> lock(state.mutex);
            if(observation.decision==Decision::StartFenceOverflow)++state.overflow;
            if(observation.decision==Decision::StartFenceFailedDiscard)++state.failedDiscard;
            if(observation.decision==Decision::StartFenceReleased)++state.released;
            state.changed.notify_all();
        };
        std::string error;require(receiver->start(error),"receiver start: "+error);
        sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);require(sender!=INVALID_SOCKET,"sender socket failed");

        receiver->prepareStart(kOverflowSession);
        for(int card=0;card<kCards;++card){receiver->beforeStartSend(card);receiver->afterStartSend(card,true);}
        int sent=0;
        for(int packet=0;packet<kOverflowPackets;++packet){
            const int result=sendPacket(sender,kDataPort,0x7100,std::uint16_t(packet),4);
            require(result==4,"overflow datagram send failed");++sent;
        }
        waitFor(state,"START Fence overflow and drain",[&state,&receiver,sent]{
            return state.overflow>0&&state.failedDiscard>0&&receiver->ingress()>=std::uint64_t(sent);
        });
        const bool complete=receiver->completeStart(true);
        int overflow=0,failedDiscard=0,released=0,cardFrames=0,syncFrames=0;
        {std::lock_guard<std::mutex> lock(state.mutex);overflow=state.overflow;failedDiscard=state.failedDiscard;
            released=state.released;cardFrames=state.cardFrames;syncFrames=state.syncFrames;}
        require(!complete,"overflow transaction completed successfully");
        require(overflow>0&&failedDiscard>0,"overflow discard evidence missing");
        require(released==0&&cardFrames==0&&syncFrames==0,"overflow transaction produced output");

        receiver->prepareStart(kRecoverySession);
        for(int card=0;card<kCards;++card){receiver->beforeStartSend(card);receiver->afterStartSend(card,true);}
        require(receiver->completeStart(true),"recovery START completion failed");
        for(int card=0;card<kCards;++card){
            const int result=sendPacket(sender,std::uint16_t(kDataPort+card),0x7101,0,1444);
            require(result==1444,"recovery datagram send failed");
        }
        waitFor(state,"overflow recovery output",[&state]{return state.recoveryFrames==kCards&&state.recoverySync>=1;});
        receiver->stop();closesocket(sender);sender=INVALID_SOCKET;receiver.reset();if(wsaStarted)WSACleanup();
        std::cout<<"overflowObserved=true completeStartReturnedFalse=true overflowCount="<<overflow
                 <<" failedDiscardCount="<<failedDiscard<<" releasedCount="<<released
                 <<" cardFrameCount="<<cardFrames<<" syncFrameCount="<<syncFrames
                 <<" recoveryPassed=true\n";
        return 0;
    }catch(const std::exception& error){
        if(receiver)receiver->stop();if(sender!=INVALID_SOCKET)closesocket(sender);if(wsaStarted)WSACleanup();
        std::cerr<<"FAIL "<<error.what()<<'\n';return 1;
    }
}
