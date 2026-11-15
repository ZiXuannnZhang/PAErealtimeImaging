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
// A queued FIFO prefix may contain control packets and samples. A later
// control backlog must neither block a covered timeout nor hide a sample.
bool feedbackPrefixCase(bool sample){
    SOCKET sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(sender==INVALID_SOCKET)return false;
    const int base=sample?29410:29420;
    std::atomic<int> stage{0},budgets{0},timeouts{0},complete{0},controls{0};
    std::atomic<bool> gate{false},release{false},cancel{false};
    SocketReceiver r({1,360,32,0},{{std::uint16_t(base+1),"127.0.0.1"}},
        {std::uint16_t(base),"127.0.0.1"},{"127.0.0.1"},nullptr,nullptr,nullptr,
        [&](Frame f){if(f&&f->complete)++complete;},{});
    r.observationSink=[&](const Observation& o){if(o.decision==Decision::Timeout)++timeouts;};
    r.feedbackSink=[&](int,int,Time){++controls;};
    r.testLoopHook=[&](std::uint64_t){if(gate.exchange(false)){stage=1;while(!release.load()&&!cancel.load())std::this_thread::yield();}
        else if(!sample&&stage.load()==5){stage=6;while(!cancel.load())std::this_thread::yield();}};
    r.testBudgetHook=[&](int card){if(card==-1){const int n=++budgets;if(n==1){stage=2;while(stage.load()==2&&!cancel.load())std::this_thread::yield();}
        else if(n==2){stage=4;while(stage.load()==4&&!cancel.load())std::this_thread::yield();}}};
    auto wait=[&](auto predicate){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);while(!predicate()&&std::chrono::steady_clock::now()<end)std::this_thread::yield();return predicate();};
    auto send=[&](int port,int size,int packet){std::vector<char> b(size);if(size>=4){b[0]=char(packet);b[2]=7;}sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);return sendto(sender,b.data(),int(b.size()),0,(sockaddr*)&a,sizeof(a))==int(b.size());};
    std::string error;bool ok=r.start(error);if(ok){r.prepareStart(1);ok=r.completeStart(true);}
    if(ok)ok=send(base+1,1444,0)&&wait([&]{return r.ingress()>=1;});
    if(ok){gate=true;ok=wait([&]{return stage.load()==1;});}
    if(ok&&!sample)std::this_thread::sleep_for(std::chrono::milliseconds(120));
    if(ok){for(int i=0;i<300;++i)ok=send(base,18,0)&&ok;if(sample)ok=send(base,1444,1)&&ok;}
    release=true;
    if(ok)ok=wait([&]{return stage.load()==2;});
    if(ok&&sample)std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if(ok&&!sample)for(int i=0;i<600;++i)ok=send(base,18,0)&&ok;
    stage=3;
    if(ok&&!sample){ok=wait([&]{return stage.load()==4;});stage=5;
        if(ok)ok=wait([&]{return stage.load()==6;})&&timeouts.load()==1&&controls.load()==512&&r.ingressIdleWatermark()>0&&r.cardIdleWatermarks()[0]>0;}
    if(ok&&sample)ok=wait([&]{return complete.load()==1;})&&timeouts.load()==0;
    cancel=true;stage=5;release=true;r.stop();closesocket(sender);
    return ok&&r.hardErrors()==0;
}
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
    closesocket(s);const bool controls=feedbackPrefixCase(false),sample=feedbackPrefixCase(true);WSACleanup();return controls&&sample&&sent&&budgets>0&&pendingChecks>0&&other==6&&frames==4&&timeouts==0&&stoppedWatermarks&&restarted&&reachedIdle&&fenceWatermarks&&r.hardErrors()==0?0:1;
}
