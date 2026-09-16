#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mswsock.h>
#include <mstcpip.h>
#include "PaimageAcquisition/SocketReceiver.h"
#include "PaimageAcquisition/SocketTimestamp.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

#ifndef SO_TIMESTAMP
#define SO_TIMESTAMP 0x300A
#endif
namespace paimage {
Time SocketReceiver::now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
SocketReceiver::SocketReceiver(Config c,std::vector<Endpoint> e,Endpoint feedback,std::vector<std::string> targets,
    TraceWriter* trace,TimingWriter* timing,LoopLog* loopLog,SourceCore::CardSink card,SourceCore::SyncSink sync)
 :config_(c),endpoints_(std::move(e)),feedback_(std::move(feedback)),targets_(std::move(targets)),trace_(trace),timing_(timing),loopLog_(loopLog),
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
    // Setup observation: creation, nonblocking mode, receive buffer and bind
    // are each recorded with the effective value or error code, before the
    // receiver thread exists. These are observation points only.
    timestampEnabled_=false;recvMsgFunction_=0;timestampStatus_=socketTimestampModeName(config_.socketTimestampMode);
    bool timestampRequested=config_.socketTimestampMode!=SocketTimestampMode::Off;
    bool timestampReady=true;
    auto make=[&](Endpoint e,bool buffer,bool feedback)->SOCKET{
        LoopRecord setup;setup.timeNs=now();setup.threadId=GetCurrentThreadId();
        setup.kind=std::uint16_t(LoopKind::SocketSetup);setup.value0=e.port;
        if(feedback)setup.flags|=1u;
        int firstError=0;
        SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(s==INVALID_SOCKET){firstError=WSAGetLastError();setup.flags|=2u;setup.value3=std::uint32_t(firstError);
            if(loopLog_)loopLog_->push(setup);
            return s;}
        setup.value1=std::uint32_t(std::uintptr_t(s)&0xffffffffu);
        if(timestampRequested){
            const auto timestamp=configureSocketTimestamp(std::uintptr_t(s),config_.socketTimestampMode);
            if(timestamp.enabled&&timestamp.recvMsgFunction){
                recvMsgFunction_=timestamp.recvMsgFunction;
                timestampStatus_=timestamp.status+(timestamp.fallback?" (fallback)":"");
            }else{
                timestampReady=false;timestampStatus_=timestamp.status;
            }
        }
        if(buffer){int bytes=64*1024*1024;
            if(setsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&bytes),sizeof(bytes))){firstError=WSAGetLastError();setup.flags|=4u;setup.value3=std::uint32_t(firstError);
                error="SO_RCVBUF "+std::to_string(firstError);if(loopLog_)loopLog_->push(setup);
                closesocket(s);return INVALID_SOCKET;}
            int n=sizeof(bytes);if(getsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&bytes),&n))bytes=-1;
            setup.flags|=16u;receiveBuffers_.push_back(bytes);setup.value2=std::uint32_t(bytes);}
        u_long nonblocking=1;if(ioctlsocket(s,FIONBIO,&nonblocking)){firstError=WSAGetLastError();setup.flags|=8u;setup.value3=std::uint32_t(firstError);
            if(loopLog_)loopLog_->push(setup);
            closesocket(s);return INVALID_SOCKET;}
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(e.port);
        if(!e.bindIp.empty()&&inet_pton(AF_INET,e.bindIp.c_str(),&local.sin_addr)!=1){error="invalid bind address";setup.flags|=32u;
            if(loopLog_)loopLog_->push(setup);
            closesocket(s);return INVALID_SOCKET;}
        if(bind(s,reinterpret_cast<sockaddr*>(&local),sizeof(local))){firstError=WSAGetLastError();setup.flags|=32u;setup.value3=std::uint32_t(firstError);
            error="bind "+std::to_string(e.port)+" error "+std::to_string(firstError);
            if(loopLog_)loopLog_->push(setup);
            closesocket(s);return INVALID_SOCKET;}
        if(loopLog_)loopLog_->push(setup);
        return s;};
    receiveBuffers_.clear();
    // 137360 precedes data setup; feedback is also configured for 64MiB
    // (137a4e..137a75), because it can carry acquisition datagrams.
    if(feedback_.port){feedbackSocket_=make(feedback_,true,true);if(feedbackSocket_==INVALID_SOCKET){closeSockets();return false;}
        feedbackReceiveBuffer_=receiveBuffers_.back();receiveBuffers_.pop_back();}
    for(auto e:endpoints_){auto s=make(e,true,false);if(s==INVALID_SOCKET){if(error.empty())error="data socket setup failed";closeSockets();return false;}sockets_.push_back(s);}
    targetAddresses_.clear();for(auto& ip:targets_){in_addr addr{};inet_pton(AF_INET,ip.c_str(),&addr);targetAddresses_.push_back(addr.s_addr);}
    timestampEnabled_=timestampRequested&&timestampReady&&recvMsgFunction_!=0;
    if(timestampRequested&&!timestampEnabled_&&timestampStatus_.empty())
        timestampStatus_="requested but not enabled on every socket";
    running_=true;worker_=std::thread(&SocketReceiver::run,this);return true;
}
void SocketReceiver::closeSockets(){for(auto s:sockets_)closesocket(SOCKET(s));sockets_.clear();if(feedbackSocket_!=INVALID_SOCKET){closesocket(SOCKET(feedbackSocket_));feedbackSocket_=INVALID_SOCKET;}if(wsa_){WSACleanup();wsa_=false;}}
void SocketReceiver::stop(){
    running_=false;
    if(worker_.joinable())worker_.join();
    {
        std::lock_guard<std::mutex> lock(coreMutex_);
        if(startFenceActive_||!startHold_.empty())resetStartFence(Decision::StartFenceShutdownDiscard);
    }
    closeSockets();
}
void SocketReceiver::observeShutdown(){
    std::lock_guard<std::mutex> lock(coreMutex_);
    if(startFenceActive_||!startHold_.empty())resetStartFence(Decision::StartFenceShutdownDiscard);
    core_.observeShutdown(now());
}
void SocketReceiver::observeFence(Decision decision,int card,std::uint16_t trigger,std::uint16_t packet,
                                  std::uint32_t count,Time at,std::uint64_t ingressId){
    core_.observeAdmission(decision,card,trigger,packet,count,at,ingressId);
}
void SocketReceiver::discardHeld(Decision decision){
    while(!startHold_.empty()){
        HeldDatagram held=std::move(startHold_.front());
        startHold_.pop_front();
        startHoldBytes_-=held.bytes.size();
        observeFence(decision,held.card,held.trigger,held.packet,1,now(),held.ingressId);
    }
    startHoldBytes_=0;
}
void SocketReceiver::discardHeldForCard(int card,Decision decision){
    for(auto it=startHold_.begin();it!=startHold_.end();){
        if(it->card!=card){++it;continue;}
        HeldDatagram held=std::move(*it);
        startHoldBytes_-=held.bytes.size();
        it=startHold_.erase(it);
        observeFence(decision,held.card,held.trigger,held.packet,1,now(),held.ingressId);
    }
}
void SocketReceiver::resetStartFence(Decision decision){
    if(!startHold_.empty())discardHeld(decision);
    startFenceActive_=false;startFenceFailed_=false;startFenceCallbacksSeen_=false;
    startCardStates_.clear();startHoldBytes_=0;
}
void SocketReceiver::beforeStartSend(int card){
    std::lock_guard<std::mutex> lock(coreMutex_);
    if(!startFenceActive_||card<0||std::size_t(card)>=startCardStates_.size())return;
    startFenceCallbacksSeen_=true;
    if(startFenceFailed_)return;
    auto& state=startCardStates_[std::size_t(card)];
    if(state==StartCardState::AwaitingStart)state=StartCardState::StartSendPending;
}
void SocketReceiver::afterStartSend(int card,bool success){
    std::lock_guard<std::mutex> lock(coreMutex_);
    if(!startFenceActive_||card<0||std::size_t(card)>=startCardStates_.size())return;
    startFenceCallbacksSeen_=true;
    auto& state=startCardStates_[std::size_t(card)];
    if(success&&!startFenceFailed_){state=StartCardState::StartSent;return;}
    state=StartCardState::StartFailed;startFenceFailed_=true;
    discardHeldForCard(card,Decision::StartFenceFailedDiscard);
}
bool SocketReceiver::holdStartDatagram(int card,const std::uint8_t* data,int length,Time received,
                                       std::uint64_t ingressId,std::uint32_t sourceIPv4,
                                       std::uint16_t sourcePort,std::uint16_t localPort,
                                       std::uint16_t trigger,std::uint16_t packet){
    if(startFenceFailed_){
        observeFence(Decision::StartFenceFailedDiscard,card,trigger,packet,1,now(),ingressId);
        return false;
    }
    const auto bytes=static_cast<std::size_t>(std::max(length,0));
    if(startHold_.size()>=kMaxStartHoldDatagrams||startHoldBytes_+bytes>kMaxStartHoldBytes){
        startFenceFailed_=true;
        observeFence(Decision::StartFenceOverflow,card,trigger,packet,1,now(),ingressId);
        discardHeld(Decision::StartFenceFailedDiscard);
        observeFence(Decision::StartFenceFailedDiscard,card,trigger,packet,1,now(),ingressId);
        return false;
    }
    HeldDatagram held;held.card=card;held.bytes.assign(data,data+bytes);held.receivedNs=received;
    held.ingressId=ingressId;held.sourceIPv4=sourceIPv4;held.sourcePort=sourcePort;held.localPort=localPort;
    held.trigger=trigger;held.packet=packet;startHoldBytes_+=bytes;startHold_.push_back(std::move(held));
    observeFence(Decision::StartFenceHeld,card,trigger,packet,1,received,ingressId);
    return true;
}
bool SocketReceiver::admitOrHold(int card,const std::uint8_t* data,int length,Time received,
                                  std::uint64_t ingressId,std::uint32_t sourceIPv4,std::uint16_t sourcePort,
                                  std::uint16_t localPort,std::uint16_t trigger,std::uint16_t packet){
    if(card<0||card>=config_.cards){
        core_.ingest(card,data,static_cast<std::size_t>(std::max(length,0)),received,ingressId,sourceIPv4);
        return false;
    }
    if(!startFenceActive_){
        core_.ingest(card,data,static_cast<std::size_t>(std::max(length,0)),received,ingressId,sourceIPv4);
        return false;
    }
    if(startFenceFailed_||std::size_t(card)>=startCardStates_.size()||
       startCardStates_[std::size_t(card)]==StartCardState::StartFailed){
        observeFence(Decision::StartFenceFailedDiscard,card,trigger,packet,1,now(),ingressId);
        return false;
    }
    switch(startCardStates_[std::size_t(card)]){
    case StartCardState::AwaitingStart:
        observeFence(Decision::StartFencePreStartDiscard,card,trigger,packet,1,now(),ingressId);
        return false;
    case StartCardState::StartSendPending:
    case StartCardState::StartSent:
        return holdStartDatagram(card,data,length,received,ingressId,sourceIPv4,sourcePort,localPort,trigger,packet);
    case StartCardState::StartFailed:
        break;
    }
    observeFence(Decision::StartFenceFailedDiscard,card,trigger,packet,1,now(),ingressId);
    return false;
}
void SocketReceiver::prepareStart(std::uint64_t session){auto begin=now();std::unique_lock<std::mutex> lock(coreMutex_);auto acquired=now();
    if(startFenceActive_||!startHold_.empty())resetStartFence(Decision::StartFenceResetDiscard);
    startFenceActive_=true;startFenceFailed_=false;startFenceCallbacksSeen_=false;
    startCardStates_.assign(std::size_t(config_.cards),StartCardState::AwaitingStart);startHoldBytes_=0;
    session_=session;correlation_=0;core_.prepareStart(session,acquired);auto end=now();
    timing(TimingKind::ControlMutexWait,begin,acquired,-1,0,1);timing(TimingKind::ControlMutexHold,acquired,end,-1,0,1);
    if(loopLog_){LoopRecord r;r.timeNs=begin;r.spanNs=end-begin;r.session=session;r.threadId=GetCurrentThreadId();
        r.kind=std::uint16_t(LoopKind::ControlMark);r.value0=1;r.payloadB=session;
        r.flags=std::uint16_t((core_.enabled()?1u:0u)|(core_.confirmed()?2u:0u));loopLog_->push(r);}}
bool SocketReceiver::completeStart(bool ok){auto begin=now();std::unique_lock<std::mutex> lock(coreMutex_);auto acquired=now();correlation_=0;
    bool fenceOk=ok;
    if(startFenceActive_){
        // Component callers may use prepare/complete without ControlSocket.
        // Production Backend always supplies the per-card callbacks.
        if(ok&&!startFenceCallbacksSeen_)
            for(auto& state:startCardStates_)state=StartCardState::StartSent;
        fenceOk=ok&&!startFenceFailed_&&std::all_of(startCardStates_.begin(),startCardStates_.end(),
                                                 [](StartCardState state){return state==StartCardState::StartSent;});
    }
    if(!fenceOk){
        core_.completeStart(false,acquired);
        discardHeld(Decision::StartFenceFailedDiscard);
    }else{
        core_.completeStart(true,acquired);
        while(!startHold_.empty()){
            HeldDatagram held=std::move(startHold_.front());startHold_.pop_front();startHoldBytes_-=held.bytes.size();
            observeFence(Decision::StartFenceReleased,held.card,held.trigger,held.packet,1,held.receivedNs,held.ingressId);
            core_.ingest(held.card,held.bytes.data(),held.bytes.size(),held.receivedNs,held.ingressId,held.sourceIPv4);
        }
        startHoldBytes_=0;
    }
    startFenceActive_=false;startFenceFailed_=false;startFenceCallbacksSeen_=false;startCardStates_.clear();
    auto end=now();
    timing(TimingKind::ControlMutexWait,begin,acquired,-1,0,2);timing(TimingKind::ControlMutexHold,acquired,end,-1,0,2);
    if(loopLog_){LoopRecord r;r.timeNs=begin;r.spanNs=end-begin;r.session=session_.load();r.threadId=GetCurrentThreadId();
        r.kind=std::uint16_t(LoopKind::ControlMark);r.value0=2;r.value1=fenceOk?1u:0u;r.payloadB=session_.load();
        r.flags=std::uint16_t((core_.enabled()?1u:0u)|(core_.confirmed()?2u:0u));loopLog_->push(r);}
    return fenceOk;
}
void SocketReceiver::prepareStop(){auto begin=now();std::unique_lock<std::mutex> lock(coreMutex_);auto acquired=now();
    if(startFenceActive_||!startHold_.empty())resetStartFence(Decision::StartFenceStopDiscard);
    correlation_=0;core_.prepareStop();auto end=now();
    timing(TimingKind::ControlMutexWait,begin,acquired,-1,0,3);timing(TimingKind::ControlMutexHold,acquired,end,-1,0,3);
    if(loopLog_){LoopRecord r;r.timeNs=begin;r.spanNs=end-begin;r.session=session_.load();r.threadId=GetCurrentThreadId();
        r.kind=std::uint16_t(LoopKind::ControlMark);r.value0=3;r.payloadB=session_.load();
        r.flags=std::uint16_t((core_.enabled()?1u:0u)|(core_.confirmed()?2u:0u));loopLog_->push(r);}}
void SocketReceiver::completeStop(bool ok){auto begin=now();std::unique_lock<std::mutex> lock(coreMutex_);auto acquired=now();correlation_=0;core_.completeStop(ok,acquired);auto end=now();
    timing(TimingKind::ControlMutexWait,begin,acquired,-1,0,4);timing(TimingKind::ControlMutexHold,acquired,end,-1,0,4);
    if(loopLog_){LoopRecord r;r.timeNs=begin;r.spanNs=end-begin;r.session=session_.load();r.threadId=GetCurrentThreadId();
        r.kind=std::uint16_t(LoopKind::ControlMark);r.value0=4;r.value1=ok?1u:0u;r.payloadB=session_.load();
        r.flags=std::uint16_t((core_.enabled()?1u:0u)|(core_.confirmed()?2u:0u));loopLog_->push(r);}}
void SocketReceiver::run(){
    priorityResult_=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST)?0:int(GetLastError());
    actualPriority_=GetThreadPriority(GetCurrentThread());
    std::vector<std::uint8_t> buffer(65536);
    const auto threadStart=now();timing(TimingKind::ThreadLife,threadStart,threadStart,-1,0,1,0,0,0,true);Time previousLoop=threadStart;std::uint64_t selectId=0,drainId=0,loopId=0;
    if(loopLog_){LoopRecord thread;thread.timeNs=threadStart;thread.threadId=GetCurrentThreadId();
        thread.kind=std::uint16_t(LoopKind::ThreadStart);
        thread.value0=std::uint32_t(priorityResult_.load());thread.value1=std::uint32_t(actualPriority_.load());
        loopLog_->push(thread);loopLog_->noteThreadStart(threadStart);}
    while(running_){const auto loopStart=now();timing(TimingKind::LoopGap,previousLoop,loopStart);
#ifdef PAIMAGE_SOCKET_TEST_SEAM
        if(testLoopHook)testLoopHook(loopId+1);
#endif
        std::uint32_t loopRecvSuccess=0,loopDrainAttempts=0;std::uint64_t readyMask=0;
        fd_set read;FD_ZERO(&read);if(feedbackSocket_!=INVALID_SOCKET)FD_SET(SOCKET(feedbackSocket_),&read);for(auto s:sockets_)FD_SET(SOCKET(s),&read);
        timeval timeout{0,1000};const auto selectStart=now();int status=select(0,&read,nullptr,nullptr,&timeout);const auto selectEnd=now();++selectId;
        timing(TimingKind::Select,selectStart,selectEnd,-1,0,status<0?std::uint32_t(-status):std::uint32_t(status),std::uint32_t(sockets_.size()+(feedbackSocket_!=INVALID_SOCKET)),selectId,std::uint16_t(status==0?1:status<0?2:0),status>0);
        if(status==SOCKET_ERROR){lastSocketError_=WSAGetLastError();++hardErrors_;break;}
        auto receivePacket=[&](SOCKET current,std::uint8_t* data,int capacity,sockaddr_in& source,int& sourceSize)->int{
            if(!timestampEnabled_||!recvMsgFunction_)
                return recvfrom(current,reinterpret_cast<char*>(data),capacity,0,reinterpret_cast<sockaddr*>(&source),&sourceSize);
            auto recvMsg=reinterpret_cast<LPFN_WSARECVMSG>(recvMsgFunction_);
            WSABUF payload{};payload.buf=reinterpret_cast<char*>(data);payload.len=static_cast<ULONG>(capacity);
            std::array<char,WSA_CMSG_SPACE(sizeof(std::uint64_t))> control{};
            WSAMSG message{};message.name=reinterpret_cast<sockaddr*>(&source);message.namelen=sourceSize;
            message.lpBuffers=&payload;message.dwBufferCount=1;message.Control.buf=control.data();
            message.Control.len=static_cast<ULONG>(control.size());message.dwFlags=0;
            DWORD received=0;
            const int result=recvMsg(current,&message,&received,nullptr,nullptr);
            if(result==SOCKET_ERROR)return SOCKET_ERROR;
            sourceSize=message.namelen;
            if(message.dwFlags&MSG_CTRUNC)++timestampControlTruncated_;
            for(auto cmsg=WSA_CMSG_FIRSTHDR(&message);cmsg;cmsg=WSA_CMSG_NXTHDR(&message,cmsg)){
                if(cmsg->cmsg_level==SOL_SOCKET&&cmsg->cmsg_type==SO_TIMESTAMP&&
                   cmsg->cmsg_len>=WSA_CMSG_LEN(sizeof(std::uint64_t))){++timestampedPackets_;break;}
            }
            return static_cast<int>(received);
        };
        auto drain=[&](SOCKET socket,int card,std::uint16_t port,bool feedback){
            if(!FD_ISSET(socket,&read))return;
            const auto drainStart=now();const auto drainIdentifier=++drainId;std::uint32_t attempts=0,success=0,bytes=0,exitReason=1;std::uint64_t lastIngress=0;
            readyMask|=std::uint64_t(1)<<(feedback?0:1+card);
            for(;;){++attempts;sockaddr_in source{};int sourceSize=sizeof(source);const auto recvStart=now();int n=receivePacket(socket,buffer.data(),int(buffer.size()),source,sourceSize);const auto recvEnd=now();
                if(n==SOCKET_ERROR){const int error=WSAGetLastError();exitReason=error==WSAEWOULDBLOCK?1:2;
                    // The failed call has no ingress record: its timing
                    // correlation is the drain identifier with flags bit 1
                    // set. A successful recvfrom correlates by ingress ID.
                    timing(TimingKind::Recvfrom,recvStart,recvEnd,card,port,0,error,drainIdentifier,std::uint16_t(exitReason|2u));
                    if(loopLog_){LoopRecord rf;rf.timeNs=recvStart;rf.spanNs=recvEnd-recvStart;rf.threadId=GetCurrentThreadId();
                        rf.kind=std::uint16_t(LoopKind::RecvFailure);rf.value0=port;rf.value1=std::uint32_t(error);
                        rf.flags=std::uint16_t(exitReason);rf.payloadA=drainIdentifier;rf.session=session_.load();loopLog_->push(rf);}
                    if(error!=WSAEWOULDBLOCK){lastSocketError_=error;++hardErrors_;}break;}
                ++success;bytes+=std::uint32_t(n);
                const auto ingressId=ingress_.fetch_add(1)+1;
                timing(TimingKind::Recvfrom,recvStart,recvEnd,card,port,std::uint32_t(n),0,ingressId,0);
                auto time=now();
                int admissionCard=card;
                if(feedback){auto it=std::find(targetAddresses_.begin(),targetAddresses_.end(),source.sin_addr.s_addr);
                    admissionCard=it==targetAddresses_.end()?-1:int(it-targetAddresses_.begin());}
                card=admissionCard;
                TraceRecord r;r.monotonicNs=time;r.session=session_;r.correlation=ingressId;r.threadId=GetCurrentThreadId();r.sourceIPv4=source.sin_addr.s_addr;
                r.card=std::int16_t(card);
                r.localPort=port;r.sourcePort=ntohs(source.sin_port);r.length=std::uint16_t(n);r.stage=1;
                std::memcpy(r.header,buffer.data(),std::min(n,4));if(n>=4){r.packet=std::uint16_t(buffer[0]|unsigned(buffer[1])<<8);r.trigger=std::uint16_t(buffer[2]|unsigned(buffer[3])<<8);}
                const auto traceStart=now();if(trace_)trace_->push(r);const auto traceEnd=now();timing(TimingKind::TracePush,traceStart,traceEnd,card,port,1,0,ingressId); // strictly before demux, parsing, admission, dedup
                if(feedback){if(n<=64){int type=feedbackType(buffer.data(),n);r.stage=3;r.reason=std::uint8_t(type);if(trace_)trace_->push(r);if(card>=0&&type&&feedbackSink)feedbackSink(card,type,time);continue;}}
                // Sampling data is every data-port datagram plus feedback-port
                // acquisition datagrams; ready/ACK feedback is not a sample.
                if(loopLog_)loopLog_->noteSampleData(time);
                const auto ingressStart=now();if(ingressSink)ingressSink(card,r);const auto ingressEnd=now();timing(TimingKind::IngressSink,ingressStart,ingressEnd,card,port,1,0,ingressId);
                const auto waitStart=now();std::unique_lock<std::mutex> lock(coreMutex_);const auto acquired=now();correlation_=ingressId;
                admitOrHold(card,buffer.data(),n,time,ingressId,source.sin_addr.s_addr,ntohs(source.sin_port),port,r.trigger,r.packet);
                const auto ingestEnd=now();
                timing(TimingKind::CoreMutexWait,waitStart,acquired,card,port,1,0,ingressId);timing(TimingKind::CoreIngest,acquired,ingestEnd,card,port,1,0,ingressId);
                lastIngress=ingressId;
            }
            const auto drainEnd=now();timing(TimingKind::Drain,drainStart,drainEnd,card,port,success,bytes,drainIdentifier,std::uint16_t(exitReason),true);
            loopRecvSuccess+=success;loopDrainAttempts+=attempts;
            if(loopLog_){LoopRecord d;d.timeNs=drainStart;d.spanNs=drainEnd-drainStart;d.threadId=GetCurrentThreadId();
                d.kind=std::uint16_t(LoopKind::Drain);d.value0=feedback?0u:std::uint32_t(card+1);d.value1=port;
                d.value2=attempts;d.value3=success;d.payloadA=bytes;d.payloadB=drainIdentifier;d.payloadC=lastIngress;
                d.flags=std::uint16_t(exitReason);d.session=session_.load();loopLog_->push(d);}
        };
        if(status>0){if(feedbackSocket_!=INVALID_SOCKET)drain(SOCKET(feedbackSocket_),-1,feedback_.port,true);
            for(std::size_t i=0;i<sockets_.size();++i)drain(SOCKET(sockets_[i]),int(i),endpoints_[i].port,false);}
        Time pollSpan=0;
        {const auto waitStart=now();std::unique_lock<std::mutex> lock(coreMutex_);const auto acquired=now();correlation_=0;core_.poll(acquired);const auto end=now();timing(TimingKind::CoreMutexWait,waitStart,acquired,-1,0,2);timing(TimingKind::CorePoll,acquired,end);pollSpan=end-waitStart;}
        if(loopLog_){LoopRecord loop;loop.timeNs=loopStart;loop.spanNs=loopStart-previousLoop;loop.threadId=GetCurrentThreadId();
            loop.kind=std::uint16_t(LoopKind::Loop);loop.value0=std::uint32_t(++loopId);
            loop.value1=std::uint32_t(std::int32_t(status));loop.value2=loopDrainAttempts;loop.value3=loopRecvSuccess;
            loop.payloadA=selectEnd-selectStart;loop.payloadB=pollSpan;loop.payloadC=readyMask;
            loop.flags=std::uint16_t(status==0?1u:(status<0?2u:0u));loop.session=session_.load();loopLog_->push(loop);}
        previousLoop=loopStart;
    }
    const auto threadEnd=now();timing(TimingKind::ThreadLife,threadStart,threadEnd,-1,0,2,0,0,0,true);
    running_=false;
}
}
