#include <winsock2.h>
#include <ws2tcpip.h>
#include "NetworkController.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QElapsedTimer>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <chrono>
#include <mutex>
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
template<class F> bool until(F f){QElapsedTimer timer;timer.start();while(!f()&&timer.elapsed()<5000){QCoreApplication::processEvents();QThread::msleep(1);}return f();}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    const bool stress=argc>=3;const int hz=argc>=5?std::stoi(argv[4]):40;
    const int triggers=stress?std::stoi(argv[1])*hz:22;
    const bool missing=argc>=4&&std::string(argv[3])=="missing";
    const int logicalTriggersPerPhase = triggers - 1; // first visible operational control is filtered
    FILETIME creation{},exit{},kernelBefore{},userBefore{},kernelAfter{},userAfter{};
    GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernelBefore,&userBefore);
    const auto wallBefore=std::chrono::steady_clock::now();
    const std::vector<int> durations=stress?std::vector<int>{std::stoi(argv[2])*4}:std::vector<int>{20000,50000,20000};
    double maxLateMs=0;
    WSADATA w{};require(WSAStartup(MAKEWORD(2,2),&w)==0,"WSAStartup");
    std::vector<SOCKET> hardware;AcqConfig config;config.nCards=4;config.acqTimeNs=20000;config.localBindIP="127.0.0.1";
    config.acqTimeNs=durations.front();
    config.diagnosticTraceEnabled=!(argc>=4&&std::string(argv[3])=="off");
    config.diagnosticLevel=argc>=4&&std::string(argv[3])=="raw"?0:1;
    for(int c=0;c<4;++c){auto ip=QString("127.0.0.%1").arg(c+2).toStdString();config.targetIPs.push_back(ip);
        SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(8080);inet_pton(AF_INET,ip.c_str(),&address.sin_addr);
        require(bind(s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"hardware control bind");DWORD timeout=2000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));hardware.push_back(s);
    }
    auto readCommands=[&](paimage::Command expected){for(auto s:hardware){paimage::Command actual{};int n=recv(s,reinterpret_cast<char*>(actual.data()),58,0);require(n==58&&actual==expected,"production wire command");}};
    auto send=[&](int c,int port,const std::vector<unsigned char>& bytes){sockaddr_in dst{};dst.sin_family=AF_INET;dst.sin_port=htons(port);inet_pton(AF_INET,"127.0.0.1",&dst.sin_addr);
        require(sendto(hardware[c],reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&dst),sizeof(dst))==int(bytes.size()),"hardware data send");};
    NetworkController controller;std::atomic<int> rings{0};std::atomic<bool> values{true};int started=0;bool stopped=false;
    std::mutex ringIdentityMutex;
    std::vector<std::vector<std::uint16_t>> ringTriggers(4);
    std::vector<std::vector<std::int64_t>> ringIndices(4);
    QObject::connect(&controller,&NetworkController::errorOccurred,[](const QString& error){std::cerr<<error.toStdString()<<'\n';});
    controller.setRingFeedSink([&](const TriggerGroupConstPtr& frame){
        if(!frame)return ImagingSubmitResult::InvalidFrame;const int c=frame->cardId;const auto& a=frame->freqA;const auto& b=frame->freqB;
        if(stress){if(a.empty()||b.empty()||a.front()!=c+1||a.back()!=c+1||b.front()!=-c-1||b.back()!=-c-1)values=false;}
        else for(std::size_t i=0;i<a.size();++i)if(a[i]!=c+1||b[i]!=-c-1)values=false;
        if(c>=0&&c<4){std::lock_guard<std::mutex> lock(ringIdentityMutex);
            ringTriggers[c].push_back(frame->triggerSeq);
            ringIndices[c].push_back(frame->logicalTriggerIndex);}
        ++rings;return ImagingSubmitResult::Accepted;});
    QObject::connect(&controller,&NetworkController::measurementStarted,[&]{++started;});
    QObject::connect(&controller,&NetworkController::stopped,[&]{stopped=true;});
    require(controller.start(config),"production listen");
    require(!controller.sendStartMeasure(),"unconfigured START must fail and remain recoverable");
    QTemporaryDir dir(QDir::currentPath()+"/network-save-XXXXXX");require(dir.isValid(),"save directory");
    if(!stress){controller.startSaving(dir.path(),1000,"switch");require(until([&]{return controller.isSaving();}),"saving applied");}
    int phase=0;
    for(int ns:durations){
        require(controller.sendConfigCommand(ns,1000,1000),"configure accepted");
        require(controller.sendStartMeasure(),"start queued behind CONFIG");readCommands(paimage::configCommand(ns,1000,1000));
        for(int c=0;c<4;++c)send(c,8000,std::vector<unsigned char>(60));
        require(until([&]{return started==phase+1;}),"ACK starts measurement");readCommands(paimage::startCommand());
        auto schedule=std::chrono::steady_clock::now();
        for(int trigger=0;trigger<triggers;++trigger){
            auto deadline=schedule+std::chrono::microseconds(trigger*(1000000ll/hz));std::this_thread::sleep_until(deadline);
            maxLateMs=std::max(maxLateMs,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-deadline).count());
            for(int c=0;c<4;++c)for(int offset=0;offset<ns*2;offset+=1440){
                if(missing&&trigger==30&&c==0&&offset==10*1440)continue;
                int n=std::min(1440,ns*2-offset);std::vector<unsigned char> p(n+4);p[0]=offset/1440;p[2]=trigger&255;p[3]=(trigger>>8)&255;
                for(int i=0;i<n;i+=8){int a=c+1,b=-a;std::memcpy(p.data()+4+i,&b,4);std::memcpy(p.data()+8+i,&a,4);}send(c,8001+c,p);
            }
            QCoreApplication::processEvents();
        }
        require(until([&]{return rings.load()==(phase+1)*logicalTriggersPerPhase*4-(missing?4:0);}),"source confirmation releases host outputs");
        if(!missing){
            std::lock_guard<std::mutex> lock(ringIdentityMutex);
            for(int c=0;c<4;++c){
                const std::size_t base=static_cast<std::size_t>(phase)*logicalTriggersPerPhase;
                require(ringTriggers[c].size()>=base+logicalTriggersPerPhase,"Ring logical identity count");
                require(ringIndices[c].size()>=base+logicalTriggersPerPhase,"Ring logical index count");
                for(int i=0;i<logicalTriggersPerPhase;++i){
                    require(ringTriggers[c][base+i]==static_cast<std::uint16_t>(i+1),"Ring physical/logical trigger identity");
                    require(ringIndices[c][base+i]==i,"Ring logical index reset");
                }
            }
        }
        require(controller.sendStopMeasure(),"production Stop");readCommands(paimage::stopCommand());
        ++phase;
    }
    if(!stress){
        unsigned tailCount=0;
        auto tail=[&]{++tailCount;for(int c=0;c<4;++c)send(c,8001+c,std::vector<unsigned char>{0,0,99,0});
            require(until([&]{auto stats=controller.getAllCardStats();for(const auto& s:stats)if(s.sessionBoundaryPacketsDiscarded<tailCount)return false;return true;}),"disabled tails observed before parsing admission");};
        for(int cycle=0;cycle<100;++cycle){tail();require(controller.sendStartMeasure(),"100-cycle production START");readCommands(paimage::startCommand());
            require(controller.sendStopMeasure(),"100-cycle production STOP");readCommands(paimage::stopCommand());tail();}
        require(rings==3*logicalTriggersPerPhase*4,"disabled tails never reach Ring");
    }
    controller.stop();require(until([&]{return stopped;}),"asynchronous listener shutdown");require(values,"Ring raw values");
    if(!stress)for(int c=0;c<4;++c)for(int phase=0;phase<3;++phase)for(const auto& channel:{QString("A"),QString("B")}){
        QFile file(dir.filePath(QString("Card%1_Ch%2_switch_%3.dat").arg(c+1).arg(channel).arg(phase,3,10,QChar('0'))));
        require(file.open(QIODevice::ReadOnly),"save sequence retained across sample changes");
        require(file.size()==qint64(logicalTriggersPerPhase)*(phase==1?12500:5000)*2,"sample-specific saved file size");
    }
    for(auto s:hardware)closesocket(s);WSACleanup();
    GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernelAfter,&userAfter);
    auto ticks=[](FILETIME f){return (std::uint64_t(f.dwHighDateTime)<<32)|f.dwLowDateTime;};
    const auto cpuMs=(ticks(kernelAfter)-ticks(kernelBefore)+ticks(userAfter)-ticks(userBefore))/10000.0;
    std::cout<<"cpuMs="<<cpuMs<<" wallMs="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wallBefore).count()
             <<" traceEnabled="<<config.diagnosticTraceEnabled<<" injectedMissing="<<missing<<'\n';
    std::cout<<"hostRingReturns="<<rings<<" maxSenderLateMs="<<maxLateMs<<" stress="<<stress<<'\n';
    std::cout<<(stress?"PASS production NetworkController stress (saving disabled)":"PASS actual NetworkController: 4-card UDP, CONFIG ACK then START, 20->50->20, source startup, Ring and retained saving files, async Stop")<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
