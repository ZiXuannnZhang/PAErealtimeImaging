#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "PaimageAcquisition/SocketReceiver.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
using namespace paimage;
int main(){
    WSADATA w{};if(WSAStartup(MAKEWORD(2,2),&w))return 1;
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(s==INVALID_SOCKET)return 2;
    std::mutex m;std::condition_variable cv;bool gate=false;std::atomic<int> frames{0},timeouts{0},budgets{0},pendingChecks{0},other{0};
    std::vector<SocketReceiver::Endpoint> ep;for(int i=0;i<4;++i)ep.push_back({std::uint16_t(29301+i),"127.0.0.1"});
    SocketReceiver r({4,360,32,0},ep,{29300,"127.0.0.1"},{"127.0.0.1"},nullptr,nullptr,nullptr,
        [&](Frame f){if(f&&f->complete)++frames;},[](auto,const auto&,bool){});
    r.testBudgetHook=[&](int card){if(card==0&&budgets.fetch_add(1)==0)std::this_thread::sleep_for(std::chrono::milliseconds(150));};
    r.observationSink=[&](const Observation& o){if(o.card==0&&o.decision==Decision::Timeout)++timeouts;};
    r.ingressSink=[&](int card,const TraceRecord&){if(card>0)++other;};
    r.testLoopHook=[&](std::uint64_t loop){if(loop==1){std::unique_lock<std::mutex> l(m);cv.wait(l,[&]{return gate;});}else if(budgets>0 && r.cardIdleWatermarks()[0]==0)++pendingChecks;};
    std::string error;if(!r.start(error)){std::cerr<<error;return 3;}r.prepareStart(1);r.completeStart(true);
    auto send=[&](int port,int packet){std::array<char,1444> b{};b[0]=char(packet);b[2]=7;sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);return sendto(s,b.data(),int(b.size()),0,(sockaddr*)&a,sizeof(a))==int(b.size());};
    bool sent=true;for(int i=0;i<300;++i)sent=send(29301,0)&&sent;sent=send(29301,1)&&sent;
    for(int c=1;c<4;++c){sent=send(29301+c,0)&&sent;sent=send(29301+c,1)&&sent;}
    {std::lock_guard<std::mutex> l(m);gate=true;}cv.notify_all();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline&&frames.load()<4)std::this_thread::yield();
    const auto before=std::chrono::steady_clock::now();r.stop();const auto stopMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-before).count();
    const bool stoppedWatermarks=r.ingressIdleWatermark()==0&&r.cardIdleWatermarks()[0]==0;
    r.testLoopHook={};r.testBudgetHook={};
    bool restarted=r.start(error);
    const auto idleDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(restarted&&r.ingressIdleWatermark()==0&&std::chrono::steady_clock::now()<idleDeadline)std::this_thread::yield();
    const bool reachedIdle=r.ingressIdleWatermark()>0;
    r.prepareStart(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool fenceWatermarks=r.ingressIdleWatermark()==0&&r.cardIdleWatermarks()[0]==0;
    r.completeStart(true);r.stop();
    std::cout<<"sent="<<sent<<" budget="<<budgets<<" pendingChecks="<<pendingChecks<<" otherIngress="<<other<<" frames="<<frames<<" prematureTimeout="<<timeouts<<" hardErrors="<<r.hardErrors()<<" stopMs="<<stopMs<<"\n";
    closesocket(s);WSACleanup();return sent&&budgets>0&&pendingChecks>0&&other==6&&frames==4&&timeouts==0&&stoppedWatermarks&&restarted&&reachedIdle&&fenceWatermarks&&r.hardErrors()==0?0:1;
}
