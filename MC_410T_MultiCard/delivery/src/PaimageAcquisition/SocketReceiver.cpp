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
    TraceWriter* trace,SourceCore::CardSink card,SourceCore::SyncSink sync)
 :config_(c),endpoints_(std::move(e)),feedback_(std::move(feedback)),targets_(std::move(targets)),trace_(trace),
  core_(c,std::move(card),std::move(sync),[this](const Observation& o){if(trace_){TraceRecord r;
      r.monotonicNs=o.time;r.session=session_.load();r.correlation=o.firstIngressId?o.firstIngressId:correlation_;r.threadId=GetCurrentThreadId();
      r.card=std::int16_t(o.card);r.trigger=o.trigger;r.packet=o.packet;r.value=o.count;r.stage=2;r.reason=std::uint8_t(o.decision);trace_->push(r);}if(observationSink)observationSink(o);}){}
SocketReceiver::~SocketReceiver(){stop();}
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
void SocketReceiver::prepareStart(std::uint64_t session){std::lock_guard<std::mutex> lock(coreMutex_);session_=session;correlation_=0;core_.prepareStart(session,now());}
void SocketReceiver::completeStart(bool ok){std::lock_guard<std::mutex> lock(coreMutex_);correlation_=0;core_.completeStart(ok);}
void SocketReceiver::prepareStop(){std::lock_guard<std::mutex> lock(coreMutex_);correlation_=0;core_.prepareStop();}
void SocketReceiver::completeStop(bool ok){std::lock_guard<std::mutex> lock(coreMutex_);correlation_=0;core_.completeStop(ok,now());}
void SocketReceiver::run(){
    priorityResult_=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST)?0:int(GetLastError());
    actualPriority_=GetThreadPriority(GetCurrentThread());
    std::vector<std::uint8_t> buffer(65536);
    while(running_){fd_set read;FD_ZERO(&read);if(feedbackSocket_!=INVALID_SOCKET)FD_SET(SOCKET(feedbackSocket_),&read);for(auto s:sockets_)FD_SET(SOCKET(s),&read);
        timeval timeout{0,1000};int status=select(0,&read,nullptr,nullptr,&timeout);
        if(status==SOCKET_ERROR){lastSocketError_=WSAGetLastError();++hardErrors_;break;}
        auto drain=[&](SOCKET socket,int card,std::uint16_t port,bool feedback){
            if(!FD_ISSET(socket,&read))return;
            for(;;){sockaddr_in source{};int sourceSize=sizeof(source);int n=recvfrom(socket,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&source),&sourceSize);
                if(n==SOCKET_ERROR){const int error=WSAGetLastError();if(error!=WSAEWOULDBLOCK){lastSocketError_=error;++hardErrors_;}break;}
                auto time=now();auto id=ingress_.fetch_add(1)+1;
                TraceRecord r;r.monotonicNs=time;r.session=session_;r.correlation=id;r.threadId=GetCurrentThreadId();r.sourceIPv4=source.sin_addr.s_addr;
                r.localPort=port;r.sourcePort=ntohs(source.sin_port);r.length=std::uint16_t(n);r.stage=1;
                std::memcpy(r.header,buffer.data(),std::min(n,4));if(n>=4){r.packet=std::uint16_t(buffer[0]|unsigned(buffer[1])<<8);r.trigger=std::uint16_t(buffer[2]|unsigned(buffer[3])<<8);}
                if(trace_)trace_->push(r); // strictly before demux, parsing, admission, dedup
                if(feedback){auto it=std::find(targetAddresses_.begin(),targetAddresses_.end(),source.sin_addr.s_addr);card=it==targetAddresses_.end()?-1:int(it-targetAddresses_.begin());
                    if(n<=64){int type=feedbackType(buffer.data(),n);r.stage=3;r.reason=std::uint8_t(type);r.card=std::int16_t(card);if(trace_)trace_->push(r);if(card>=0&&type&&feedbackSink)feedbackSink(card,type);continue;}}
                if(ingressSink)ingressSink(card,r);
                std::lock_guard<std::mutex> lock(coreMutex_);correlation_=id;core_.ingest(card,buffer.data(),n,time,id,source.sin_addr.s_addr);
            }
        };
        if(status>0){if(feedbackSocket_!=INVALID_SOCKET)drain(SOCKET(feedbackSocket_),-1,feedback_.port,true);
            for(std::size_t i=0;i<sockets_.size();++i)drain(SOCKET(sockets_[i]),int(i),endpoints_[i].port,false);}
        {std::lock_guard<std::mutex> lock(coreMutex_);correlation_=0;core_.poll(now());}
    }
    running_=false;
}
}
