#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "PaimageAcquisition/ControlSocket.h"
#include "PaimageAcquisition/ControlState.h"
#include "PaimageAcquisition/SocketReceiver.h"
#include "PaimageAcquisition/TraceWriter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
using namespace paimage;

constexpr int kCards=4;
constexpr int kPackets=8;
constexpr int kPrefixPackets=3;
constexpr int kSamples=1440;
constexpr std::uint16_t kDataPort=28001;
constexpr std::uint16_t kControlPort=28080;
constexpr int kSessionsPerScenario=20;
constexpr auto kWait=std::chrono::seconds(5);

enum class Scenario { WholeTrigger, PrefixTrigger, NormalAfterStart };

const char* scenarioName(Scenario scenario){
    switch(scenario){
    case Scenario::WholeTrigger:return "whole_trigger";
    case Scenario::PrefixTrigger:return "prefix_trigger";
    case Scenario::NormalAfterStart:return "normal_after_start";
    }
    return "unknown";
}

struct Plan {
    std::uint64_t session=0;
    Scenario scenario=Scenario::WholeTrigger;
    std::uint16_t firstTrigger=0,secondTrigger=0;
    int prePackets=0;
    bool injectPreStartStale=false;
};

struct SessionResult {
    Plan plan;
    int rawIngress=0,rawPre=0,heldPre=0,releasedPre=0,preStartDiscard=0,failedDiscard=0;
    int heldIngress=0,releasedIngress=0,postBoundaryDisabled=0,syncFrames=0;
    int completeFrames=0,partialFrames=0,crossSessionFrameLeak=0,doubleRelease=0;
    bool startSucceeded=false,passed=false;
};

struct SessionMetrics {
    int rawIngress=0,heldIngress=0,releasedIngress=0,preStartDiscard=0,failedDiscard=0;
    int postBoundaryDisabled=0,completeFrames=0,partialFrames=0,syncFrames=0;
    int crossSessionFrameLeak=0,doubleRelease=0;
    std::unordered_set<std::uint64_t> releaseIngressIds;
};

struct FailureResult {
    std::uint64_t session=9000;
    bool startReturnedFalse=false,fenceReturnedFalse=false,recoverySessionPassed=false;
    int heldCount=0,failedDiscardCount=0,releasedCount=0,cardFrameCount=0,syncFrameCount=0;
};

struct HarnessState {
    std::mutex mutex;
    std::condition_variable changed;
    Plan plan;
    bool postGo=false,failureMode=false;
    std::array<bool,kCards> startReceived{},preSent{},postSent{};
    std::array<int,kCards> heldPre{};
    std::vector<int> startOrder;
    std::vector<Frame> frames;
    int rawPre=0,preStartDiscard=0,failedDiscard=0,releasedPre=0;
    std::map<std::uint64_t,SessionMetrics> metrics;
};

void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}

class RaceHarness {
public:
    explicit RaceHarness(std::filesystem::path root):root_(std::move(root)),trace_(root_){
        std::filesystem::create_directories(root_);
        const std::array<std::string,kCards> targets={"127.0.0.2","127.0.0.3","127.0.0.4","127.0.0.5"};
        for(int card=0;card<kCards;++card)openCard(card,targets[card]);
        std::string error;
        require(control_.open("127.0.0.1",std::vector<std::string>(targets.begin(),targets.end()),kControlPort,error),
                "control open: "+error);
        std::vector<SocketReceiver::Endpoint> endpoints;
        for(int card=0;card<kCards;++card)endpoints.push_back({std::uint16_t(kDataPort+card),"127.0.0.1"});
        receiver_=std::make_unique<SocketReceiver>(
            Config{kCards,kSamples,32,0},endpoints,SocketReceiver::Endpoint{0,{}},
            std::vector<std::string>(targets.begin(),targets.end()),&trace_,nullptr,nullptr,
            [this](Frame frame){onFrame(std::move(frame));},
            [this](std::uint16_t,const std::vector<Frame>& frames,bool){onSync(frames);});
        receiver_->ingressSink=[this](int card,const TraceRecord& record){onIngress(card,record);};
        receiver_->observationSink=[this](const Observation& observation){onObservation(observation);};
        controlState_=std::make_unique<ControlState>(kCards,
            [this](const Command& command,const std::vector<int>& cards){
                return control_.send(command,cards,
                    [this](const Command& bytes,const ControlSocket::SendResult& sent){
                        if(bytes[4]==3&&bytes[57]==1)
                            receiver_->afterStartSend(sent.card,sent.bytes==int(bytes.size())&&sent.error==0);
                        if(bytes[4]!=3||bytes[57]!=1)return;
                        TraceRecord record{};record.monotonicNs=SocketReceiver::now();record.session=currentSession();
                        record.threadId=GetCurrentThreadId();record.card=std::int16_t(sent.card);
                        record.sourceIPv4=sent.targetIPv4;record.localPort=control_.localPort();record.sourcePort=kControlPort;
                        record.length=sent.bytes<0?0:std::uint16_t(sent.bytes);record.value=std::uint32_t(sent.error);
                        record.packet=bytes[57];record.stage=6;record.reason=bytes[4];trace_.push(record);
                    },
                    [this](const Command& bytes,int card){
                        if(bytes[4]==3&&bytes[57]==1)receiver_->beforeStartSend(card);
                    });
            },SocketReceiver::now);
    }
    ~RaceHarness(){shutdown();}

    void start(){
        std::string error;require(receiver_->start(error),"receiver start: "+error);
        for(auto& card:cards_)card.thread=std::thread(&RaceHarness::cardLoop,this,card.index);
        controlState_->setListening(true);
        require(controlState_->configure(configCommand(20000,1000,1000),SocketReceiver::now(),false),"CONFIG send failed");
        waitFor("all cards receive CONFIG",[this]{return std::all_of(configReceived_.begin(),configReceived_.end(),[](bool value){return value;});});
#ifdef PAIMAGE_SOCKET_TEST_SEAM
        control_.setTestSendHook([this](const Command& command,const ControlSocket::SendResult& sent){
            if(command[4]==3&&command[57]==1)onStartSend(sent);
        });
#else
        throw std::runtime_error("PAIMAGE_SOCKET_TEST_SEAM is required");
#endif
    }

    std::vector<SessionResult> runScenarios(){
        std::vector<SessionResult> results;std::uint64_t session=1000;
        const std::array<Scenario,3> scenarios={Scenario::WholeTrigger,Scenario::PrefixTrigger,Scenario::NormalAfterStart};
        for(Scenario scenario:scenarios)for(int repetition=0;repetition<kSessionsPerScenario;++repetition){
            Plan plan;plan.session=session++;plan.scenario=scenario;plan.prePackets=scenario==Scenario::WholeTrigger?kPackets:scenario==Scenario::PrefixTrigger?kPrefixPackets:0;
            const std::uint16_t base=repetition<4?std::uint16_t(0xfffe + repetition):
                std::uint16_t((scenario==Scenario::WholeTrigger?100:scenario==Scenario::PrefixTrigger?200:300)+repetition*2);
            plan.firstTrigger=base;plan.secondTrigger=std::uint16_t(base+1);plan.injectPreStartStale=plan.session==1000;
            results.push_back(runOne(plan));
        }
        results.push_back(runFailureAndRecovery());
        return results;
    }

    const std::filesystem::path& root()const{return root_;}
    std::uint64_t receiverHardErrors()const{return receiver_?receiver_->hardErrors():0;}
    std::uint64_t emulatorSendErrors()const{return emulatorSendErrors_.load();}
    std::uint64_t traceQueueDropped()const{return trace_.dropped();}
    bool traceIncomplete()const{return trace_.incomplete();}
    std::map<std::uint64_t,SessionMetrics> metrics(){
        std::lock_guard<std::mutex> lock(state_.mutex);return state_.metrics;
    }
    FailureResult failureResult()const{return failure_;}

    void shutdown(){
        if(shutdown_)return;shutdown_=true;stopping_=true;
        {std::lock_guard<std::mutex> lock(state_.mutex);state_.changed.notify_all();}
        if(receiver_)receiver_->stop();
        for(auto& card:cards_)if(card.thread.joinable())card.thread.join();
        for(auto& card:cards_)if(card.socket!=INVALID_SOCKET){closesocket(card.socket);card.socket=INVALID_SOCKET;}
        if(controlState_)controlState_->setListening(false);
        controlState_.reset();control_.close();trace_.stop();
    }

private:
    struct CardSocket {int index=-1;SOCKET socket=INVALID_SOCKET;std::thread thread;};

    std::uint64_t currentSession(){std::lock_guard<std::mutex> lock(state_.mutex);return state_.plan.session;}

    void openCard(int card,const std::string& ip){
        auto& target=cards_[card];target.index=card;target.socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        require(target.socket!=INVALID_SOCKET,"card emulator socket create failed");
        DWORD timeout=100;setsockopt(target.socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(kControlPort);
        require(inet_pton(AF_INET,ip.c_str(),&address.sin_addr)==1,"card emulator IP conversion failed");
        require(bind(target.socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,
                "card emulator bind failed for "+ip);
    }

    template<class Predicate>
    void waitFor(const std::string& label,Predicate predicate){
    std::unique_lock<std::mutex> lock(state_.mutex);
    const bool completed=state_.changed.wait_for(lock,kWait,[&]{return predicate()||stopping_.load()||emulatorSendErrors_.load()!=0;});
        if(!completed){const auto it=state_.metrics.find(state_.plan.session);const auto raw=it==state_.metrics.end()?0:it->second.rawIngress;
            const auto failed=it==state_.metrics.end()?0:it->second.failedDiscard;
            throw std::runtime_error("timeout: "+label+" session="+std::to_string(state_.plan.session)+
                " raw="+std::to_string(raw)+" failedDiscard="+std::to_string(failed));}
        require(!stopping_.load(),"harness stopped while waiting for "+label);
        const std::string failure=emulatorFailure_;
        require(emulatorSendErrors_.load()==0,"card emulator send error while waiting for "+label+
            " wsa="+std::to_string(emulatorLastError_.load())+" detail="+failure);
        require(predicate(),"incomplete condition: "+label);
    }

    void prepare(const Plan& plan){
        std::lock_guard<std::mutex> lock(state_.mutex);state_.plan=plan;state_.postGo=false;state_.failureMode=false;
        state_.startReceived.fill(false);state_.preSent.fill(plan.prePackets==0);state_.postSent.fill(false);
        state_.heldPre.fill(0);state_.startOrder.clear();state_.rawPre=0;
        state_.preStartDiscard=0;state_.failedDiscard=0;state_.releasedPre=0;
        state_.metrics[plan.session]=SessionMetrics{};
    }

    SessionResult runOne(const Plan& plan){
        prepare(plan);
        bool fenceOk=true;
        const bool started=controlState_->start([this,plan]{receiver_->prepareStart(plan.session);},
            [this,&fenceOk](bool ok){fenceOk=receiver_->completeStart(ok);});
        require(started&&fenceOk,"START transaction failed for session "+std::to_string(plan.session));
        waitFor("all cards receive START",[this]{return std::all_of(state_.startReceived.begin(),state_.startReceived.end(),[](bool value){return value;});});
        {std::lock_guard<std::mutex> lock(state_.mutex);state_.postGo=true;state_.changed.notify_all();}
        const int expectedFrames=kCards*2;
        waitFor("post packets and complete frames",[this,plan,expectedFrames]{
            const bool allSent=std::all_of(state_.postSent.begin(),state_.postSent.end(),[](bool value){return value;});
            int frames=0;for(const auto& frame:state_.frames)if(frame&&frame->measurementSession==plan.session)++frames;
            return allSent&&frames>=expectedFrames&&state_.metrics[plan.session].syncFrames>=2;
        });
        return snapshot(plan,true);
    }

    SessionResult runFailureAndRecovery(){
        Plan failed;failed.session=9000;failed.scenario=Scenario::WholeTrigger;failed.firstTrigger=5000;failed.secondTrigger=5001;failed.prePackets=kPackets;
        prepare(failed);{std::lock_guard<std::mutex> lock(state_.mutex);state_.failureMode=true;}
#ifdef PAIMAGE_SOCKET_TEST_SEAM
        control_.setTestSendResultHook([](const Command& command,ControlSocket::SendResult& result){
            if(command[4]==3&&command[57]==1&&result.card==2){result.bytes=0;result.error=WSAECONNREFUSED;}
        });
#endif
        bool fenceOk=true;
        const bool started=controlState_->start([this,failed]{receiver_->prepareStart(failed.session);},
            [this,&fenceOk](bool ok){fenceOk=receiver_->completeStart(ok);});
        require(!started&&!fenceOk,"START send failure unexpectedly succeeded");
        failure_.startReturnedFalse=!started;failure_.fenceReturnedFalse=!fenceOk;
        {
            std::lock_guard<std::mutex> lock(state_.mutex);state_.postGo=true;state_.changed.notify_all();
        }
        waitFor("held data discarded after START failure",[this]{return state_.failedDiscard>0;});
        waitFor("failed card workers released",[this]{return std::all_of(state_.postSent.begin(),state_.postSent.end(),[](bool value){return value;});});
        waitFor("failed session ingress drained",[this,failed]{
            const auto& metrics=state_.metrics[failed.session];
            return metrics.rawIngress>=kPackets*kCards&&
                metrics.failedDiscard+metrics.postBoundaryDisabled>=kPackets*kCards;
        });
        {
            std::lock_guard<std::mutex> lock(state_.mutex);state_.failureMode=false;
        }
#ifdef PAIMAGE_SOCKET_TEST_SEAM
        control_.setTestSendResultHook({});
#endif
        {
            std::lock_guard<std::mutex> lock(state_.mutex);
            const auto& metrics=state_.metrics[failed.session];
            failure_.heldCount=metrics.heldIngress;failure_.failedDiscardCount=metrics.failedDiscard;
            failure_.releasedCount=metrics.releasedIngress;failure_.cardFrameCount=metrics.completeFrames+metrics.partialFrames;
            failure_.syncFrameCount=metrics.syncFrames;
        }
        require(failure_.startReturnedFalse,"failure session did not return START=false");
        require(failure_.fenceReturnedFalse,"failure session fence did not return false");
        require(failure_.heldCount>0,"failure session produced no held ingress");
        require(failure_.failedDiscardCount>0,"failure session produced no failed discard");
        require(failure_.releasedCount==0,"failure session released held ingress");
        require(failure_.cardFrameCount==0,"failure session produced CardFrame output");
        require(failure_.syncFrameCount==0,"failure session produced SyncFrame output");
        Plan recovery;recovery.session=9001;recovery.scenario=Scenario::NormalAfterStart;recovery.firstTrigger=0;recovery.secondTrigger=1;recovery.prePackets=0;
        SessionResult result=runOne(recovery);failure_.recoverySessionPassed=result.passed;
        require(failure_.recoverySessionPassed,"recovery session did not pass after START failure");
        require(result.passed,"recovery session failed after START failure raw="+std::to_string(result.rawIngress)+
            " held="+std::to_string(result.heldIngress)+" released="+std::to_string(result.releasedIngress)+
            " disabled="+std::to_string(result.postBoundaryDisabled)+" complete="+std::to_string(result.completeFrames)+
            " partial="+std::to_string(result.partialFrames)+" sync="+std::to_string(result.syncFrames)+
            " crossSession="+std::to_string(result.crossSessionFrameLeak)+" doubleRelease="+std::to_string(result.doubleRelease));return result;
    }

    SessionResult snapshot(const Plan& plan,bool startSucceeded){
        std::lock_guard<std::mutex> lock(state_.mutex);SessionResult result;result.plan=plan;result.startSucceeded=startSucceeded;
        result.rawPre=state_.rawPre;result.preStartDiscard=state_.preStartDiscard;result.failedDiscard=state_.failedDiscard;result.releasedPre=state_.releasedPre;
        const auto& metrics=state_.metrics[plan.session];result.rawIngress=metrics.rawIngress;result.heldIngress=metrics.heldIngress;
        result.releasedIngress=metrics.releasedIngress;result.postBoundaryDisabled=metrics.postBoundaryDisabled;
        result.syncFrames=metrics.syncFrames;result.crossSessionFrameLeak=metrics.crossSessionFrameLeak;result.doubleRelease=metrics.doubleRelease;
        for(int value:state_.heldPre)result.heldPre+=value;
        for(const auto& frame:state_.frames)if(frame&&frame->measurementSession==plan.session){if(frame->complete)++result.completeFrames;else ++result.partialFrames;}
        const int expectedPre=plan.prePackets*kCards;
        const bool exactPre=plan.prePackets==0||(result.rawPre==expectedPre&&result.heldPre==expectedPre&&result.releasedPre==expectedPre);
        result.passed=result.startSucceeded&&exactPre&&result.preStartDiscard==(plan.injectPreStartStale?1:0)&&result.failedDiscard==0&&
            result.postBoundaryDisabled==0&&result.completeFrames==kCards*2&&result.partialFrames==0&&result.syncFrames==2&&
            result.crossSessionFrameLeak==0&&result.doubleRelease==0;
        return result;
    }

    void onStartSend(const ControlSocket::SendResult& sent){
        Plan plan;bool failureMode=false;
        {std::lock_guard<std::mutex> lock(state_.mutex);plan=state_.plan;failureMode=state_.failureMode;state_.startOrder.push_back(sent.card);state_.changed.notify_all();}
        if(failureMode){
            if(sent.bytes==58&&sent.error==0&&plan.prePackets>0)
                waitFor("failure pre-trigger raw ingress",[this]{return state_.rawPre>0;});
            return;
        }
        if(sent.bytes!=58||sent.error!=0||plan.prePackets==0)return;
        if(plan.injectPreStartStale&&sent.card==0){
            sendPackets(3,0xff00,0,1);
            waitFor("pre-card-START stale discard",[this]{return state_.preStartDiscard>=1;});
        }
        waitFor("card pre-trigger raw ingress",[this,sent,plan]{return state_.preSent[sent.card]&&state_.heldPre[sent.card]>=plan.prePackets;});
    }

    void cardLoop(int card){
        try{
            while(!stopping_.load()){
                std::array<std::uint8_t,sizeof(Command)> command{};sockaddr_in source{};int sourceSize=sizeof(source);
                const int count=recvfrom(cards_[card].socket,reinterpret_cast<char*>(command.data()),int(command.size()),0,
                                         reinterpret_cast<sockaddr*>(&source),&sourceSize);
                if(count==SOCKET_ERROR){const int error=WSAGetLastError();if(error==WSAETIMEDOUT||error==WSAEWOULDBLOCK)continue;
                    emulatorLastError_=error;emulatorSendErrors_++;continue;}
                if(count!=int(sizeof(Command)))continue;
                if(command[4]==2){std::lock_guard<std::mutex> lock(state_.mutex);configReceived_[card]=true;state_.changed.notify_all();continue;}
                if(command[4]!=3||command[57]!=1)continue;
                Plan plan;{std::lock_guard<std::mutex> lock(state_.mutex);plan=state_.plan;state_.startReceived[card]=true;state_.changed.notify_all();}
                if(plan.scenario==Scenario::WholeTrigger)sendPackets(card,plan.firstTrigger,0,kPackets);
                else if(plan.scenario==Scenario::PrefixTrigger)sendPackets(card,plan.firstTrigger,0,kPrefixPackets);
                {std::lock_guard<std::mutex> lock(state_.mutex);state_.preSent[card]=true;state_.changed.notify_all();}
                waitFor("post release",[this,plan]{return state_.plan.session==plan.session&&state_.postGo;});
                {std::lock_guard<std::mutex> lock(state_.mutex);if(state_.failureMode){state_.postSent[card]=true;state_.changed.notify_all();continue;}}
                if(plan.scenario==Scenario::WholeTrigger)sendPackets(card,plan.secondTrigger,0,kPackets);
                else if(plan.scenario==Scenario::PrefixTrigger){sendPackets(card,plan.firstTrigger,kPrefixPackets,kPackets);sendPackets(card,plan.secondTrigger,0,kPackets);}
                else {sendPackets(card,plan.firstTrigger,0,kPackets);sendPackets(card,plan.secondTrigger,0,kPackets);}
                {std::lock_guard<std::mutex> lock(state_.mutex);state_.postSent[card]=true;state_.changed.notify_all();}
            }
        }catch(const std::exception& error){
            if(!stopping_.load()){
                {std::lock_guard<std::mutex> lock(state_.mutex);emulatorFailure_=error.what();}
                emulatorSendErrors_++;
            }
            state_.changed.notify_all();
        }catch(...){
            if(!stopping_.load()){
                {std::lock_guard<std::mutex> lock(state_.mutex);emulatorFailure_="unknown exception";}
                emulatorSendErrors_++;
            }
            state_.changed.notify_all();
        }
    }

    void sendPackets(int card,std::uint16_t trigger,int begin,int end){
        std::vector<std::uint8_t> packet(4+1440,0);sockaddr_in destination{};destination.sin_family=AF_INET;
        destination.sin_port=htons(std::uint16_t(kDataPort+card));inet_pton(AF_INET,"127.0.0.1",&destination.sin_addr);
        for(int number=begin;number<end;++number){packet[0]=std::uint8_t(number&255);packet[1]=std::uint8_t(number>>8);
            packet[2]=std::uint8_t(trigger&255);packet[3]=std::uint8_t(trigger>>8);
            bool delivered=false;
            for(int attempt=0;attempt<100;++attempt){
                const int sent=sendto(cards_[card].socket,reinterpret_cast<const char*>(packet.data()),int(packet.size()),0,
                                      reinterpret_cast<sockaddr*>(&destination),sizeof(destination));
                if(sent==int(packet.size())){delivered=true;break;}
                const int error=WSAGetLastError();
                if(error!=WSAENOBUFS&&error!=WSAEWOULDBLOCK&&error!=WSATRY_AGAIN)break;
                Sleep(1);
            }
            if(!delivered){emulatorLastError_=WSAGetLastError();++emulatorSendErrors_;}
        }
    }

    void onIngress(int card,const TraceRecord& record){
        std::lock_guard<std::mutex> lock(state_.mutex);auto& metrics=state_.metrics[record.session];++metrics.rawIngress;
        if(record.session!=state_.plan.session||card<0||card>=kCards)return;
        if(record.trigger==state_.plan.firstTrigger&&record.packet<std::uint16_t(state_.plan.prePackets))++state_.rawPre;
        state_.changed.notify_all();
    }

    void onObservation(const Observation& observation){
        std::lock_guard<std::mutex> lock(state_.mutex);auto& metrics=state_.metrics[state_.plan.session];
        if(observation.decision==Decision::Disabled)++metrics.postBoundaryDisabled;
        if(observation.decision==Decision::StartFenceHeld)++metrics.heldIngress;
        if(observation.decision==Decision::StartFenceReleased){
            ++metrics.releasedIngress;
            if(observation.firstIngressId&&
               !metrics.releaseIngressIds.insert(observation.firstIngressId).second)++metrics.doubleRelease;
        }
        if(observation.decision==Decision::StartFencePreStartDiscard)++metrics.preStartDiscard;
        if(observation.decision==Decision::StartFenceFailedDiscard)++metrics.failedDiscard;
        if(observation.card<0||observation.card>=kCards)return;
        if(observation.trigger==state_.plan.firstTrigger&&observation.packet<std::uint16_t(state_.plan.prePackets)){
            if(observation.decision==Decision::StartFenceHeld)++state_.heldPre[observation.card];
            if(observation.decision==Decision::StartFenceReleased)++state_.releasedPre;
        }
        if(observation.decision==Decision::StartFencePreStartDiscard)++state_.preStartDiscard;
        if(observation.decision==Decision::StartFenceFailedDiscard)++state_.failedDiscard;
        state_.changed.notify_all();
    }

    void onFrame(Frame frame){
        if(!frame)return;std::lock_guard<std::mutex> lock(state_.mutex);auto& metrics=state_.metrics[frame->measurementSession];
        if(frame->complete)++metrics.completeFrames;else ++metrics.partialFrames;
        state_.frames.push_back(std::move(frame));state_.changed.notify_all();
    }

    void onSync(const std::vector<Frame>& frames){
        std::lock_guard<std::mutex> lock(state_.mutex);auto& metrics=state_.metrics[state_.plan.session];++metrics.syncFrames;
        for(const auto& frame:frames)if(frame&&frame->measurementSession!=state_.plan.session)++metrics.crossSessionFrameLeak;
        state_.changed.notify_all();
    }

    std::filesystem::path root_;TraceWriter trace_;ControlSocket control_;std::unique_ptr<SocketReceiver> receiver_;std::unique_ptr<ControlState> controlState_;
    HarnessState state_;std::array<CardSocket,kCards> cards_{};std::array<bool,kCards> configReceived_{};
    FailureResult failure_;
    std::atomic<bool> stopping_{false};std::atomic<std::uint64_t> emulatorSendErrors_{0};std::atomic<int> emulatorLastError_{0};std::string emulatorFailure_;bool shutdown_=false;
};

void writeResult(const std::filesystem::path& root,const std::vector<SessionResult>& results,
                 const std::map<std::uint64_t,SessionMetrics>& metrics,const FailureResult& failure,
                 std::uint64_t hardErrors,std::uint64_t emulatorErrors,std::uint64_t traceDropped,bool traceIncomplete,bool passed){
    int success=0,whole=0,prefix=0,normal=0,wrapNear=0,raw=0,held=0,released=0,disabled=0,complete=0,partial=0,sync=0,leaks=0,doubleRelease=0;
    for(const auto& result:results){
        if(result.plan.session<1000||result.plan.session>=1060)continue;
        ++success;if(result.plan.firstTrigger>=0xfffe)++wrapNear;
        if(result.plan.scenario==Scenario::WholeTrigger)++whole;
        else if(result.plan.scenario==Scenario::PrefixTrigger)++prefix;
        else ++normal;
        const auto it=metrics.find(result.plan.session);if(it==metrics.end())continue;
        raw+=it->second.rawIngress;held+=it->second.heldIngress;released+=it->second.releasedIngress;
        disabled+=it->second.postBoundaryDisabled;complete+=it->second.completeFrames;partial+=it->second.partialFrames;
        sync+=it->second.syncFrames;leaks+=it->second.crossSessionFrameLeak;doubleRelease+=it->second.doubleRelease;
    }
    std::ofstream output(root/"result.json");output<<"{\n  \"sessionCount\":"<<results.size()<<",\n  \"successSessionCount\":"<<success
        <<",\n  \"wholeTriggerSessionCount\":"<<whole<<",\n  \"prefixTriggerSessionCount\":"<<prefix
        <<",\n  \"normalAfterStartSessionCount\":"<<normal<<",\n  \"wrapNearSessionCount\":"<<wrapNear
        <<",\n  \"rawIngressCount\":"<<raw<<",\n  \"heldIngressCount\":"<<held<<",\n  \"releasedIngressCount\":"<<released
        <<",\n  \"postBoundaryDisabledCount\":"<<disabled<<",\n  \"completeFrameCount\":"<<complete
        <<",\n  \"partialFrameCount\":"<<partial<<",\n  \"syncFrameCount\":"<<sync
        <<",\n  \"crossSessionFrameLeakCount\":"<<leaks<<",\n  \"doubleReleaseCount\":"<<doubleRelease
        <<",\n  \"failureSession\":{\"startReturnedFalse\":"<<(failure.startReturnedFalse?"true":"false")
        <<",\"fenceReturnedFalse\":"<<(failure.fenceReturnedFalse?"true":"false")<<",\"heldCount\":"<<failure.heldCount
        <<",\"failedDiscardCount\":"<<failure.failedDiscardCount<<",\"releasedCount\":"<<failure.releasedCount
        <<",\"cardFrameCount\":"<<failure.cardFrameCount<<",\"syncFrameCount\":"<<failure.syncFrameCount
        <<",\"recoverySessionPassed\":"<<(failure.recoverySessionPassed?"true":"false")<<"},\n  \"scenarios\":[\n";
    for(std::size_t i=0;i<results.size();++i){const auto& result=results[i];output<<"    {\"session\":"<<result.plan.session<<",\"scenario\":\""<<scenarioName(result.plan.scenario)
        <<"\",\"rawIngress\":"<<result.rawIngress<<",\"rawPre\":"<<result.rawPre<<",\"heldPre\":"<<result.heldPre<<",\"releasedPre\":"<<result.releasedPre
        <<",\"heldIngress\":"<<result.heldIngress<<",\"releasedIngress\":"<<result.releasedIngress
        <<",\"postBoundaryDisabled\":"<<result.postBoundaryDisabled<<",\"preStartDiscard\":"<<result.preStartDiscard<<",\"failedDiscard\":"<<result.failedDiscard
        <<",\"completeFrames\":"<<result.completeFrames<<",\"partialFrames\":"<<result.partialFrames<<",\"syncFrames\":"<<result.syncFrames
        <<",\"crossSessionFrameLeak\":"<<result.crossSessionFrameLeak<<",\"doubleRelease\":"<<result.doubleRelease
        <<",\"passed\":"<<(result.passed?"true":"false")<<"}"<<(i+1==results.size()?"":" ,")<<"\n";}
    output<<"  ],\n  \"hardSocketErrors\":"<<hardErrors<<",\n  \"emulatorSendErrors\":"<<emulatorErrors
        <<",\n  \"traceQueueDropped\":"<<traceDropped<<",\n  \"traceIncomplete\":"<<(traceIncomplete?"true":"false")<<",\n  \"passed\":"<<(passed?"true":"false")<<"\n}\n";
}

std::filesystem::path outputRoot(int argc,char** argv){std::filesystem::path parent="start-race-artifacts";for(int i=1;i+1<argc;i+=2)if(std::string(argv[i])=="--output")parent=argv[i+1];
    const auto stamp=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();return parent/("run-"+std::to_string(stamp));}

} // namespace

int main(int argc,char** argv){
    const auto root=outputRoot(argc,argv);WSADATA winsock{};bool started=false;RaceHarness* harness=nullptr;
    try{
        require(WSAStartup(MAKEWORD(2,2),&winsock)==0,"WSAStartup failed");started=true;
        RaceHarness instance(root);harness=&instance;instance.start();const auto results=instance.runScenarios();instance.shutdown();
        bool passed=instance.receiverHardErrors()==0&&instance.emulatorSendErrors()==0&&instance.traceQueueDropped()==0&&!instance.traceIncomplete();
        for(const auto& result:results)passed=passed&&result.passed;
        writeResult(root,results,instance.metrics(),instance.failureResult(),instance.receiverHardErrors(),instance.emulatorSendErrors(),instance.traceQueueDropped(),instance.traceIncomplete(),passed);
        std::cout<<"sessions="<<results.size()<<" hardSocketErrors="<<instance.receiverHardErrors()<<" emulatorSendErrors="<<instance.emulatorSendErrors()<<" traceQueueDropped="<<instance.traceQueueDropped()<<" traceIncomplete="<<(instance.traceIncomplete()?"true":"false")<<"\n";
        std::cout<<(passed?"PASS deterministic paimage START Fence UDP integration\n":"FAIL deterministic paimage START Fence UDP integration\n");
        if(started)WSACleanup();return passed?0:1;
    }catch(const std::exception& error){if(harness)harness->shutdown();std::cerr<<"FAIL "<<error.what()<<"\n";if(started)WSACleanup();return 1;}
}
