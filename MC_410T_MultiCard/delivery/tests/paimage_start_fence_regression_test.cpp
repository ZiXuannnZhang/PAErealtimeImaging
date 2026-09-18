#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "PaimageAcquisition/SocketReceiver.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace paimage;
constexpr int kCards=4;
constexpr std::uint16_t kDataPort=29001;
constexpr std::uint64_t kFailedSession=7000;
constexpr std::uint64_t kRecoverySession=7001;

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    int disabled=0, released=0, failedFrames=0, failedSync=0, recoveryFrames=0, recoverySync=0;
};

void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}

template<class Predicate>
void waitFor(State& state,const std::string& label,Predicate predicate){
    std::unique_lock<std::mutex> lock(state.mutex);
    require(state.changed.wait_for(lock,std::chrono::seconds(5),predicate),"timeout: "+label);
}

void sendPacket(SOCKET sender,std::uint16_t port,std::uint16_t trigger,std::uint16_t packet){
    std::array<std::uint8_t,1444> bytes{};
    bytes[0]=std::uint8_t(packet&255);bytes[1]=std::uint8_t(packet>>8);
    bytes[2]=std::uint8_t(trigger&255);bytes[3]=std::uint8_t(trigger>>8);
    sockaddr_in destination{};destination.sin_family=AF_INET;destination.sin_port=htons(port);
    require(inet_pton(AF_INET,"127.0.0.1",&destination.sin_addr)==1,"destination conversion failed");
    const int sent=sendto(sender,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,
                          reinterpret_cast<sockaddr*>(&destination),sizeof(destination));
    require(sent==int(bytes.size()),"recovery datagram send failed");
}
}

int main(){
    WSADATA wsa{};SOCKET sender=INVALID_SOCKET;std::unique_ptr<SocketReceiver> receiver;
    bool wsaStarted=false;
    try{
        require(WSAStartup(MAKEWORD(2,2),&wsa)==0,"WSAStartup failed");wsaStarted=true;
        State state;
        std::vector<SocketReceiver::Endpoint> endpoints;
        for(int card=0;card<kCards;++card)endpoints.push_back({std::uint16_t(kDataPort+card),"127.0.0.1"});
        std::vector<std::string> targets(kCards,"127.0.0.1");
        receiver=std::make_unique<SocketReceiver>(
            Config{kCards,180,32,0},endpoints,SocketReceiver::Endpoint{0,{}},targets,
            nullptr,nullptr,nullptr,
            [&state](Frame frame){
                if(!frame)return;std::lock_guard<std::mutex> lock(state.mutex);
                if(frame->measurementSession==kFailedSession)++state.failedFrames;
                if(frame->measurementSession==kRecoverySession)++state.recoveryFrames;
                state.changed.notify_all();
            },
            [&state](std::uint16_t,const std::vector<Frame>& frames,bool){
                std::lock_guard<std::mutex> lock(state.mutex);
                for(const auto& frame:frames)if(frame&&frame->measurementSession==kFailedSession)++state.failedSync;
                for(const auto& frame:frames)if(frame&&frame->measurementSession==kRecoverySession){++state.recoverySync;break;}
                state.changed.notify_all();
            });
        receiver->observationSink=[&state](const Observation& observation){
            std::lock_guard<std::mutex> lock(state.mutex);
            if(observation.decision==Decision::Disabled)++state.disabled;
            if(observation.decision==Decision::StartFenceReleased)++state.released;
            state.changed.notify_all();
        };
        std::string error;require(receiver->start(error),"receiver start: "+error);
        sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);require(sender!=INVALID_SOCKET,"sender socket failed");

        receiver->prepareStart(kFailedSession);
        const bool failedResult=receiver->completeStart(false);
        require(!failedResult,"completeStart(false) returned true");
        sendPacket(sender,kDataPort,0x7000,0);
        waitFor(state,"disabled admission after failed START",[&state]{return state.disabled>0;});
        int failedFrames=0,failedSync=0,released=0;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            failedFrames=state.failedFrames;failedSync=state.failedSync;released=state.released;
        }
        require(failedFrames==0&&failedSync==0,"failed START produced output");
        require(released==0,"failed START released held data");

        receiver->prepareStart(kRecoverySession);
        require(receiver->completeStart(true),"compatibility completeStart(true) failed");
        for(int card=0;card<kCards;++card)sendPacket(sender,std::uint16_t(kDataPort+card),0x7001,0);
        waitFor(state,"compatibility recovery output",[&state]{return state.recoveryFrames==kCards&&state.recoverySync>=1;});
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            require(state.failedFrames==0&&state.failedSync==0&&state.released==0,"failed session evidence changed");
        }
        receiver->stop();closesocket(sender);sender=INVALID_SOCKET;receiver.reset();
        if(wsaStarted)WSACleanup();
        std::cout<<"completeStartFalseReturnedFalse=true sourceRemainedDisabled=true releasedCount="<<released
                 <<" cardFrameCount="<<failedFrames<<" syncFrameCount="<<failedSync
                 <<" nextCompatibilityStartPassed=true\n";
        return 0;
    }catch(const std::exception& error){
        if(receiver)receiver->stop();if(sender!=INVALID_SOCKET)closesocket(sender);if(wsaStarted)WSACleanup();
        std::cerr<<"FAIL "<<error.what()<<'\n';return 1;
    }
}
