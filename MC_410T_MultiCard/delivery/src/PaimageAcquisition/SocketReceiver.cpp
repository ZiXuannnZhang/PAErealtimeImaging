#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "PaimageAcquisition/SocketReceiver.h"
#include <algorithm>
#include <chrono>
#include <cstring>
namespace paimage {
Time SocketReceiver::now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
SocketReceiver::SocketReceiver(Config c,std::vector<Endpoint> e,Endpoint feedback,std::vector<std::string> targets,
    TraceWriter* trace,TimingWriter* timing,SourceCore::CardSink card,SourceCore::SyncSink sync)
 :config_(c),endpoints_(std::move(e)),feedback_(std::move(feedback)),targets_(std::move(targets)),trace_(trace),timing_(timing),
  core_(c,std::move(card),std::move(sync),[this](const Observation& o){if(trace_){TraceRecord r;
      r.monotonicNs=o.time;r.session=session_.load();r.correlation=o.firstIngressId?o.firstIngressId:correlation_;r.threadId=GetCurrentThreadId();
      r.card=std::int16_t(o.card);r.trigger=o.trigger;r.packet=o.packet;r.value=o.count;r.stage=2;r.reason=std::uint8_t(o.decision);trace_->push(r);}if(observationSink)observationSink(o);}){}
SocketReceiver::~SocketReceiver(){stop();}
void SocketReceiver::timing(TimingKind kind,Time start,Time end,int card,std::uint16_t port,
    std::uint32_t value0,std::uint32_t value1,std::uint64_t correlation,std::uint16_t flags,bool force)noexcept{
    if(!timing_)return;
    TimingRecord r;r.startNs=start;r.endNs=end;r.session=session_.load();r.correlation=correlation;
    r.threadId=GetCurrentThreadId();r.card=std::int16_t(card);r.localPort=port;r.kind=std::uint16_t(kind);r.flags=flags;r.value0=value0;r.value1=value1;
    timing_->observe(r,force||end-start>=500000);
}
bool SocketReceiver::start(std::string& error){
    if(running_)return true;
    if(worker_.joinable()){error="receiver failed; stop listener before restarting";return false;}
    if(endpoints_.size()!=std::size_t(config_.cards)){error="card/socket count mismatch";return false;}
    WSADATA data{};int status=WSAStartup(MAKEWORD(2,2),&data);if(status){error="WSAStartup "+std::to_string(status);return false;}wsa_=true;
    auto make=[&](Endpoint e,bool buffer)->SOCKET{SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(s==INVALID_SOCKET)return s;
        if(buffer){int bytes=64*1024*1024;if(setsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&bytes),sizeof(bytes))){error="SO_RCVBUF "+std::to_string(WSAGetLastError());closesocket(s);return INVALID_SOCKET;}
            int n=sizeof(bytes);if(getsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&bytes),&n))bytes=-1;receiveBuffers_.push_back(bytes);}
        u_long nonblocking=1;if(ioctlsocket(s,FIONBIO,&nonblocking)){closesocket(s);return INVALID_SOCKET;}
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(e.port);
        if(!e.bindIp.empty()&&inet_pton(AF_INET,e.bindIp.c_str(),&local.sin_addr)!=1){error="invalid bind address";closesocket(s);return INVALID_SOCKET;}
        if(bind(s,reinterpret_cast<sockaddr*>(&local),sizeof(local))){error="bind "+std::to_string(e.port)+" error "+std::to_string(WSAGetLastError());closesocket(s);return INVALID_SOCKET;}return s;};
    receiveBuffers_.clear();
    // 137360 precedes data setup; feedback is also configured for 64MiB
    // (137a4e..137a75), because it can carry acquisition datagrams.
    if(feedback_.port){feedbackSocket_=make(feedback_,true);if(feedbackSocket_==INVALID_SOCKET){closeSockets();return false;}
        feedbackReceiveBuffer_=receiveBuffers_.back();receiveBuffers_.pop_back();}
    for(auto e:endpoints_){auto s=make(e,true);if(s==INVALID_SOCKET){if(error.empty())error="data socket setup failed";closeSockets();return false;}sockets_.push_back(s);}
    targetAddresses_.clear();for(auto& ip:targets_){in_addr addr{};inet_pton(AF_INET,ip.c_str(),&addr);targetAddresses_.push_back(addr.s_addr);}
    running_=true;worker_=std::thread(&SocketReceiver::run,this);return true;
}
void SocketReceiver::closeSockets(){for(auto s:sockets_)closesocket(SOCKET(s));sockets_.clear();if(feedbackSocket_!=INVALID_SOCKET){closesocket(SOCKET(feedbackSocket_));feedbackSocket_=INVALID_SOCKET;}if(wsa_){WSACleanup();wsa_=false;}}
void SocketReceiver::stop(){running_=false;if(worker_.joinable())worker_.join();closeSockets();}
void SocketReceiver::prepareStart(std::uint64_t session){auto begin=now();std::unique_lock<std::mutex> lock(coreMutex_);auto acquired=now();session_=session;correlation_=0;core_.prepareStart(session,acquired);auto end=now();timing(TimingKind::ControlMutexWait,begin,acquired,-1,0,1);timing(TimingKind::ControlMutexHold,acquired,end,-1,0,1);}
void SocketReceiver::completeStart(bool ok){auto begin=now();std::unique_lock<std::mutex> lock(coreMutex_);auto acquired=now();correlation_=0;core_.completeStart(ok);auto end=now();timing(TimingKind::ControlMutexWait,begin,acquired,-1,0,2);timing(TimingKind::ControlMutexHold,acquired,end,-1,0,2);}
void SocketReceiver::prepareStop(){auto begin=now();std::unique_lock<std::mutex> lock(coreMutex_);auto acquired=now();correlation_=0;core_.prepareStop();auto end=now();timing(TimingKind::ControlMutexWait,begin,acquired,-1,0,3);timing(TimingKind::ControlMutexHold,acquired,end,-1,0,3);}
void SocketReceiver::completeStop(bool ok){auto begin=now();std::unique_lock<std::mutex> lock(coreMutex_);auto acquired=now();correlation_=0;core_.completeStop(ok,acquired);auto end=now();timing(TimingKind::ControlMutexWait,begin,acquired,-1,0,4);timing(TimingKind::ControlMutexHold,acquired,end,-1,0,4);}
void SocketReceiver::run(){
    priorityResult_=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST)?0:int(GetLastError());
    actualPriority_=GetThreadPriority(GetCurrentThread());
    std::vector<std::uint8_t> buffer(65536);
    const auto threadStart=now();timing(TimingKind::ThreadLife,threadStart,threadStart,-1,0,1,0,0,0,true);Time previousLoop=threadStart;std::uint64_t selectId=0,drainId=0;
    while(running_){const auto loopStart=now();timing(TimingKind::LoopGap,previousLoop,loopStart);fd_set read;FD_ZERO(&read);if(feedbackSocket_!=INVALID_SOCKET)FD_SET(SOCKET(feedbackSocket_),&read);for(auto s:sockets_)FD_SET(SOCKET(s),&read);
        timeval timeout{0,1000};const auto selectStart=now();int status=select(0,&read,nullptr,nullptr,&timeout);const auto selectEnd=now();++selectId;
        timing(TimingKind::Select,selectStart,selectEnd,-1,0,status<0?std::uint32_t(-status):std::uint32_t(status),std::uint32_t(sockets_.size()+(feedbackSocket_!=INVALID_SOCKET)),selectId,std::uint16_t(status==0?1:status<0?2:0),status>0);
        if(status==SOCKET_ERROR){lastSocketError_=WSAGetLastError();++hardErrors_;break;}
        auto drain=[&](SOCKET socket,int card,std::uint16_t port,bool feedback){
            if(!FD_ISSET(socket,&read))return;
            const auto drainStart=now();const auto id=++drainId;std::uint32_t attempts=0,success=0,bytes=0,exitReason=1;
            for(;;){++attempts;sockaddr_in source{};int sourceSize=sizeof(source);const auto recvStart=now();int n=recvfrom(socket,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&source),&sourceSize);const auto recvEnd=now();
                if(n==SOCKET_ERROR){const int error=WSAGetLastError();exitReason=error==WSAEWOULDBLOCK?1:2;timing(TimingKind::Recvfrom,recvStart,recvEnd,card,port,0,error,id,exitReason);if(error!=WSAEWOULDBLOCK){lastSocketError_=error;++hardErrors_;}break;}
                ++success;bytes+=std::uint32_t(n);timing(TimingKind::Recvfrom,recvStart,recvEnd,card,port,std::uint32_t(n),0,id,0);
                auto time=now();auto id=ingress_.fetch_add(1)+1;
                TraceRecord r;r.monotonicNs=time;r.session=session_;r.correlation=id;r.threadId=GetCurrentThreadId();r.sourceIPv4=source.sin_addr.s_addr;
                r.localPort=port;r.sourcePort=ntohs(source.sin_port);r.length=std::uint16_t(n);r.stage=1;
                std::memcpy(r.header,buffer.data(),std::min(n,4));if(n>=4){r.packet=std::uint16_t(buffer[0]|unsigned(buffer[1])<<8);r.trigger=std::uint16_t(buffer[2]|unsigned(buffer[3])<<8);}
                const auto traceStart=now();if(trace_)trace_->push(r);const auto traceEnd=now();timing(TimingKind::TracePush,traceStart,traceEnd,card,port,1,0,id); // strictly before demux, parsing, admission, dedup
                if(feedback){auto it=std::find(targetAddresses_.begin(),targetAddresses_.end(),source.sin_addr.s_addr);card=it==targetAddresses_.end()?-1:int(it-targetAddresses_.begin());
                    if(n<=64){int type=feedbackType(buffer.data(),n);r.stage=3;r.reason=std::uint8_t(type);r.card=std::int16_t(card);if(trace_)trace_->push(r);if(card>=0&&type&&feedbackSink)feedbackSink(card,type);continue;}}
                const auto ingressStart=now();if(ingressSink)ingressSink(card,r);const auto ingressEnd=now();timing(TimingKind::IngressSink,ingressStart,ingressEnd,card,port,1,0,id);
                const auto waitStart=now();std::unique_lock<std::mutex> lock(coreMutex_);const auto acquired=now();correlation_=id;core_.ingest(card,buffer.data(),n,time,id,source.sin_addr.s_addr);const auto ingestEnd=now();
                timing(TimingKind::CoreMutexWait,waitStart,acquired,card,port,1,0,id);timing(TimingKind::CoreIngest,acquired,ingestEnd,card,port,1,0,id);
            }
            const auto drainEnd=now();timing(TimingKind::Drain,drainStart,drainEnd,card,port,success,bytes,id,std::uint16_t(exitReason),true);
        };
        if(status>0){if(feedbackSocket_!=INVALID_SOCKET)drain(SOCKET(feedbackSocket_),-1,feedback_.port,true);
            for(std::size_t i=0;i<sockets_.size();++i)drain(SOCKET(sockets_[i]),int(i),endpoints_[i].port,false);}
        {const auto waitStart=now();std::unique_lock<std::mutex> lock(coreMutex_);const auto acquired=now();correlation_=0;core_.poll(acquired);const auto end=now();timing(TimingKind::CoreMutexWait,waitStart,acquired,-1,0,2);timing(TimingKind::CorePoll,acquired,end);}
        previousLoop=now();
    }
    const auto threadEnd=now();timing(TimingKind::ThreadLife,threadStart,threadEnd,-1,0,2,0,0,0,true);
    running_=false;
}
}
