#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "PaimageAcquisition/SocketReceiver.h"
#include "PaimageAcquisition/OutputWorkers.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
using namespace paimage;
double processCpuSeconds(){FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return -1;
    ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return double(k.QuadPart+u.QuadPart)/1e7;}
int main(int argc,char**argv){
    auto wallBegin=std::chrono::steady_clock::now();auto cpuBegin=processCpuSeconds();
    int seconds=2,samples=5000,missing=-1;bool diagnostics=true;std::filesystem::path out="replay";
    for(int i=1;i+1<argc;i+=2){std::string key=argv[i],value=argv[i+1];if(key=="--seconds")seconds=std::stoi(value);else if(key=="--samples")samples=std::stoi(value);else if(key=="--missing")missing=std::stoi(value);else if(key=="--trace")diagnostics=std::stoi(value)!=0;else if(key=="--out")out=value;else return 2;}
    std::filesystem::create_directories(out);
    std::unique_ptr<TraceWriter> trace;if(diagnostics)trace=std::make_unique<TraceWriter>(out/"trace");
    std::atomic<std::uint64_t> cards{0},sync{0},partial{0},badSamples{0};
    std::vector<SocketReceiver::Endpoint> endpoints;for(int i=0;i<4;++i)endpoints.push_back({std::uint16_t(18001+i),"127.0.0.1"});
    OutputWorkers outputs(4,50,
        [&](Frame f){++cards;if(!f->complete)++partial;std::vector<float>a,b;decodeRaw(*f,32,a,b);if(a.size()!=unsigned(samples)||a[0]!=20000.f||b[0]!=-10000.f)++badSamples;
            if(trace){TraceRecord r;r.stage=4;r.monotonicNs=SocketReceiver::now();r.session=f->measurementSession;r.correlation=f->firstIngressId;r.threadId=GetCurrentThreadId();r.card=std::int16_t(f->card);r.trigger=f->trigger;r.packet=f->basePacket;r.value=f->unique;r.length=std::uint16_t(f->expected);r.reason=f->complete?0:1;trace->push(r);}},
        [&](const SyncFrame&){++sync;},
        [&](OutputWorkers::Result result,Frame f){if(trace&&f){TraceRecord r;r.stage=5;r.reason=std::uint8_t(result);r.monotonicNs=SocketReceiver::now();r.session=f->measurementSession;r.correlation=f->firstIngressId;r.threadId=GetCurrentThreadId();r.card=std::int16_t(f->card);r.trigger=f->trigger;trace->push(r);}});
    outputs.start();outputs.setSavingEnabled(true);outputs.beginSession(1);
    SocketReceiver receiver({4,samples,32,1000},endpoints,{18000,"127.0.0.1"},{"127.0.0.1"},trace.get(),
        [&](Frame f){outputs.pushCard(std::move(f));},
        [&](std::uint16_t trigger,const auto& frames,bool startup){outputs.pushSync(trigger,frames,startup);});
    std::string error;if(!receiver.start(error)){std::cerr<<error;return 2;}receiver.prepareStart(1);receiver.completeStart(true);
    SOCKET sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(sender==INVALID_SOCKET)return 2;
    sockaddr_in target{};target.sin_family=AF_INET;inet_pton(AF_INET,"127.0.0.1",&target.sin_addr);
    int byteCount=samples*8,n=(byteCount+1439)/1440;std::vector<std::vector<std::uint8_t>> packets;
    for(int p=0;p<n;++p){int bytes=std::min(1440,byteCount-p*1440);std::vector<std::uint8_t>b(bytes+4);b[0]=p&255;b[1]=(p>>8)&255;
        for(int i=0;i+8<=bytes;i+=8){std::int32_t x=-10000-p*180-i/8,y=20000+p*180+i/8;std::memcpy(b.data()+4+i,&x,4);std::memcpy(b.data()+8+i,&y,4);}packets.push_back(std::move(b));}
    auto start=std::chrono::steady_clock::now();std::uint64_t sent=0,sendErrors=0;double maxLateMs=0;
    for(int t=0;t<seconds*40;++t){auto due=start+std::chrono::microseconds(t*25000ll);std::this_thread::sleep_until(due);auto now=std::chrono::steady_clock::now();maxLateMs=std::max(maxLateMs,std::chrono::duration<double,std::milli>(now-due).count());
        for(int c=0;c<4;++c){target.sin_port=htons(std::uint16_t(18001+c));for(int p=0;p<n;++p){if(t==0&&p==missing)continue;auto&b=packets[p];b[2]=t&255;b[3]=(t>>8)&255;int actual=sendto(sender,reinterpret_cast<char*>(b.data()),int(b.size()),0,reinterpret_cast<sockaddr*>(&target),sizeof(target));if(actual==int(b.size()))++sent;else ++sendErrors;}}}
    closesocket(sender);std::this_thread::sleep_for(std::chrono::milliseconds(200));receiver.prepareStop();receiver.completeStop(true);receiver.stop();outputs.stop();if(trace)trace->stop();
    auto expectedCards=std::uint64_t(seconds)*40*4;auto expectedSync=std::uint64_t(seconds)*40-(missing>=0?1:0);
    bool ok=sendErrors==0&&receiver.ingress()==sent&&cards==expectedCards&&sync==expectedSync&&badSamples==0&&receiver.hardErrors()==0&&(!trace||!trace->incomplete());
    std::ofstream result(out/"result.json");result<<"{\"testScope\":\"socket/source/component-adapter, not production GUI\",\"samples\":"<<samples<<",\"seconds\":"<<seconds<<",\"plannedHz\":40,\"sent\":"<<sent<<",\"sendErrors\":"<<sendErrors<<",\"ingress\":"<<receiver.ingress()<<",\"cardOutputs\":"<<cards.load()<<",\"syncOutputs\":"<<sync.load()<<",\"partialOutputs\":"<<partial.load()<<",\"badSamples\":"<<badSamples.load()<<",\"hardErrors\":"<<receiver.hardErrors()<<",\"maxSenderLateMs\":"<<maxLateMs<<",\"prioritySetError\":"<<receiver.priorityResult()<<",\"actualPriority\":"<<receiver.actualPriority()<<",\"traceIncomplete\":"<<(trace&&trace->incomplete()?"true":"false")<<",\"passed\":"<<(ok?"true":"false")<<"}\n";
    result.close();
    std::ofstream metrics(out/"process-metrics.json");
    auto wallSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-wallBegin).count();
    auto cpuEnd=processCpuSeconds();
    metrics<<"{\"scope\":\"whole replay process including sender, not isolated receiver\",\"diagnosticsEnabled\":"<<(diagnostics?"true":"false")
        <<",\"wallSeconds\":"<<wallSeconds<<",\"cpuSeconds\":";
    if(cpuBegin<0||cpuEnd<0)metrics<<"null";else metrics<<cpuEnd-cpuBegin;
    metrics<<",\"actualReceiveBuffersBytes\":[";auto buffers=receiver.receiveBuffers();
    for(std::size_t i=0;i<buffers.size();++i){if(i)metrics<<',';metrics<<buffers[i];}metrics<<"]}\n";
    std::cout<<(ok?"PASS":"FAIL")<<" sent="<<sent<<" ingress="<<receiver.ingress()<<" cards="<<cards<<" sync="<<sync<<" partial="<<partial<<"\n";return ok?0:1;
}
