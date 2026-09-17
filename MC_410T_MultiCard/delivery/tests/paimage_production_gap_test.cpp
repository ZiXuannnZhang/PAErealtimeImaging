#include <winsock2.h>
#include <ws2tcpip.h>
#include "NetworkController.h"
#include "PaimageAcquisition/SourceCore.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>

// Session B review addendum: deterministic production-seam proof that the
// PAimage ingress (NetworkController -> Backend -> SocketReceiver ->
// SourceCore -> observationSink) accounts full triggerSeq gaps into
// CardStats::missingTriggerCount / packetsDropped with the frozen semantics
// (T100 -> T104 == +3 triggers and +3 * expectedPackets). Loopback UDP only;
// no hardware claim.
namespace {
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
template<class F> bool until(F f){QElapsedTimer timer;timer.start();while(!f()&&timer.elapsed()<5000){QCoreApplication::processEvents();QThread::msleep(1);}return f();}

// One fake acquisition card on loopback: receives the production CONFIG /
// START / STOP wire commands and returns 60-byte config ACKs, so the real
// control transaction and start fence run unmodified.
struct FakeCard {
    SOCKET control=INVALID_SOCKET,sender=INVALID_SOCKET;
    ~FakeCard(){if(control!=INVALID_SOCKET)closesocket(control);if(sender!=INVALID_SOCKET)closesocket(sender);}
    bool open(){
        control=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(control==INVALID_SOCKET)return false;
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(8080);
        inet_pton(AF_INET,"127.0.0.2",&address.sin_addr);
        if(bind(control,reinterpret_cast<sockaddr*>(&address),sizeof(address)))return false;
        DWORD timeout=2000;setsockopt(control,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        return sender!=INVALID_SOCKET;
    }
    paimage::Command readCommand(){
        paimage::Command command{};
        const int n=recv(control,reinterpret_cast<char*>(command.data()),int(command.size()),0);
        require(n==int(command.size()),"production wire command");
        return command;
    }
    void ackConfig(){
        std::vector<unsigned char> ack(60,0);
        sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(8000);inet_pton(AF_INET,"127.0.0.1",&to.sin_addr);
        require(sendto(control,reinterpret_cast<const char*>(ack.data()),int(ack.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to))==int(ack.size()),"config ACK send");
    }
    // Header-only datagrams: 4-byte packetSeq/triggerSeq header is all the
    // production ingress needs for assembly and gap accounting.
    void packet(std::uint16_t trigger,std::uint16_t seq){
        const unsigned char p[4]={static_cast<unsigned char>(seq&255),static_cast<unsigned char>(seq>>8),
                                  static_cast<unsigned char>(trigger&255),static_cast<unsigned char>(trigger>>8)};
        sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(8001);inet_pton(AF_INET,"127.0.0.1",&to.sin_addr);
        require(sendto(sender,reinterpret_cast<const char*>(p),4,0,reinterpret_cast<sockaddr*>(&to),sizeof(to))==4,"data send");
    }
    void trigger(std::uint16_t t,int packets){for(int s=0;s<packets;++s)packet(t,static_cast<std::uint16_t>(s));}
};

struct Scenario {
    FakeCard card;
    NetworkController controller;
    std::atomic<int> started{0};
    std::atomic<bool> stopped{false};
    void begin(int acqTimeNs){
        require(card.open(),"fake card sockets");
        AcqConfig config;config.nCards=1;config.acqTimeNs=acqTimeNs;config.localBindIP="127.0.0.1";
        config.targetIPs={"127.0.0.2"};config.diagnosticTraceEnabled=false;config.diagnosticLevel=0;
        QObject::connect(&controller,&NetworkController::measurementStarted,[&]{++started;});
        QObject::connect(&controller,&NetworkController::stopped,[&]{stopped=true;});
        require(controller.start(config),"production listen");
        require(!controller.sendStartMeasure(),"unconfigured START must fail");
        require(controller.sendConfigCommand(acqTimeNs,1000,1000),"configure accepted");
        require(controller.sendStartMeasure(),"start queued behind CONFIG");
        require(card.readCommand()==paimage::configCommand(acqTimeNs,1000,1000),"CONFIG wire bytes");
        card.ackConfig();
        require(until([&]{return started==1;}),"ACK starts measurement");
        require(card.readCommand()==paimage::startCommand(),"START wire bytes");
    }
    CardStats::Snapshot stats(){
        const auto all=controller.getAllCardStats();
        require(all.size()==1,"single production card stats");
        return all.front();
    }
    void settle(){QElapsedTimer timer;timer.start();while(timer.elapsed()<300){QCoreApplication::processEvents();QThread::msleep(2);}}
    void restartMeasurement(){
        require(controller.sendStopMeasure(),"production STOP");
        require(card.readCommand()==paimage::stopCommand(),"STOP wire bytes");
        require(controller.sendStartMeasure(),"second session START");
        require(until([&]{return started==2;}),"second session started");
        require(card.readCommand()==paimage::startCommand(),"second START wire bytes");
    }
    void end(){
        require(controller.sendStopMeasure(),"production STOP");
        require(card.readCommand()==paimage::stopCommand(),"STOP wire bytes");
        controller.stop();
        require(until([&]{return stopped.load();}),"asynchronous listener shutdown");
    }
};

// B-ADD-1: complete T100 then complete T104, 1 packet per trigger.
void fullGapOnly(){
    Scenario s;s.begin(512);
    s.card.trigger(100,1);
    require(until([&]{return s.stats().triggersComplete==1;}),"T100 complete");
    s.card.trigger(104,1);
    require(until([&]{return s.stats().triggersComplete==2;}),"T104 complete");
    s.settle();
    const auto v=s.stats();
    require(v.missingTriggerCount==3,"B-ADD-1 missingTriggerCount == 3");
    require(v.packetsDropped==3,"B-ADD-1 packetsDropped == 3 * expectedPackets(1)");
    require(v.triggersPartial==0,"B-ADD-1 triggersPartial == 0");
    s.end();
    std::cout<<"PASS B-ADD-1 production full gap: T100->T104 missing=3 dropped=3 partial=0\n";
}

// B-ADD-2: partial T100 (3/6 packets) then complete T104, 6 packets per trigger.
void partialPlusGap(){
    Scenario s;s.begin(4000);
    s.card.trigger(100,3);
    s.card.trigger(104,6);
    require(until([&]{const auto v=s.stats();return v.triggersComplete==1&&v.triggersPartial==1;}),"partial close plus complete");
    s.settle();
    const auto v=s.stats();
    require(v.triggersPartial==1,"B-ADD-2 triggersPartial == 1");
    require(v.missingTriggerCount==3,"B-ADD-2 missingTriggerCount == 3");
    require(v.packetsDropped==3+3*6,"B-ADD-2 packetsDropped == partial 3 + gap 3*6");
    s.end();
    std::cout<<"PASS B-ADD-2 production partial+full gap: partial=1 missing=3 dropped=21\n";
}

// B-ADD-3: adjacent triggers never count a gap.
void adjacentNoGap(){
    Scenario s;s.begin(512);
    s.card.trigger(200,1);
    s.card.trigger(201,1);
    require(until([&]{return s.stats().triggersComplete==2;}),"adjacent complete");
    s.settle();
    const auto v=s.stats();
    require(v.missingTriggerCount==0,"B-ADD-3 missingTriggerCount == 0");
    require(v.packetsDropped==0&&v.triggersPartial==0,"B-ADD-3 no loss accounted");
    s.end();
    std::cout<<"PASS B-ADD-3 production adjacent: T200->T201 missing=0 dropped=0\n";
}

// B-ADD-4: uint16 wrap forward T65534 -> T1 == missing T65535/T0 == +2.
void wrapForward(){
    Scenario s;s.begin(512);
    s.card.trigger(65534,1);
    require(until([&]{return s.stats().triggersComplete==1;}),"T65534 complete");
    s.card.trigger(1,1);
    require(until([&]{return s.stats().triggersComplete==2;}),"T1 complete");
    s.settle();
    const auto v=s.stats();
    require(v.missingTriggerCount==2,"B-ADD-4 wrap missingTriggerCount == 2");
    require(v.packetsDropped==2,"B-ADD-4 wrap packetsDropped == 2 * expectedPackets(1)");
    s.end();
    std::cout<<"PASS B-ADD-4 production wrap forward: T65534->T1 missing=2 dropped=2\n";
}

// B-ADD-5: recent duplicate and late backstep triggers create no gap, and the
// forward anchor never retreats (T106 must stay adjacent to T105, not T102).
void backstepNoFalseGap(){
    Scenario s;s.begin(512);
    s.card.trigger(100,1);
    require(until([&]{return s.stats().triggersComplete==1;}),"T100 complete");
    s.card.trigger(105,1);
    require(until([&]{const auto v=s.stats();return v.triggersComplete==2&&v.missingTriggerCount==4;}),"real gap counted once");
    s.card.trigger(105,1); // recent duplicate of a closed trigger
    s.card.trigger(102,1); // late backstep outside the recent window
    require(until([&]{return s.stats().triggersComplete==3;}),"late trigger assembles");
    s.card.trigger(106,1);
    require(until([&]{return s.stats().triggersComplete==4;}),"post-backstep complete");
    s.settle();
    const auto v=s.stats();
    require(v.missingTriggerCount==4,"B-ADD-5 backstep created no extra gap");
    require(v.packetsDropped==4,"B-ADD-5 dropped matches the single real gap");
    require(v.staleTriggerPacketsDiscarded==1,"B-ADD-5 recent duplicate discarded");
    s.end();
    std::cout<<"PASS B-ADD-5 production backstep/stale: missing stays 4, anchor never retreated\n";
}

// B-ADD-6: a new measurement session must not inherit the old session anchor.
void sessionResetClearsAnchor(){
    Scenario s;s.begin(512);
    s.card.trigger(100,1);
    require(until([&]{return s.stats().triggersComplete==1;}),"session 1 T100 complete");
    s.restartMeasurement();
    s.card.trigger(120,1); // would be +19 if the old anchor (100) leaked across sessions
    require(until([&]{return s.stats().triggersComplete==2;}),"session 2 T120 complete");
    s.settle();
    const auto v=s.stats();
    require(v.missingTriggerCount==0,"B-ADD-6 old anchor must not pollute the new session");
    require(v.packetsDropped==0,"B-ADD-6 no cross-session gap equivalents");
    s.end();
    std::cout<<"PASS B-ADD-6 production session reset: T100 then T120 after restart, missing=0\n";
}
}

int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    WSADATA winsock{};
    if(WSAStartup(MAKEWORD(2,2),&winsock)!=0)return 2;
    try{
        fullGapOnly();
        partialPlusGap();
        adjacentNoGap();
        wrapForward();
        backstepNoFalseGap();
        sessionResetClearsAnchor();
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';WSACleanup();return 1;}
    WSACleanup();
    std::cout<<"PASS paimage production trigger-gap accounting: B-ADD-1..6\n";
    return 0;
}
