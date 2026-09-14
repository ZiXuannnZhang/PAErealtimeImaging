#include "PaimageAcquisition/Backend.h"
#include <stdexcept>
namespace paimage {
namespace {std::vector<SocketReceiver::Endpoint> endpoints(const Backend::Settings& s){
    std::vector<SocketReceiver::Endpoint> result;
    for(int c=0;c<s.acquisition.cards;++c)result.push_back({std::uint16_t(s.dataPort+c),{}});
    return result;
}}
Backend::Backend(Settings settings,std::vector<DataProcessor*> p,std::vector<FileSaver*> s,TraceWriter* trace,TimingWriter* timing,LoopLog* loopLog)
    :settings_(std::move(settings)),trace_(trace),timing_(timing),
     output_(settings_.acquisition.bits,settings_.blockSize,std::move(p),std::move(s),trace,timing,
             settings_.logicalTriggersPerRound,std::move(settings_.normalizerObserver)),
     receiver_(settings_.acquisition,endpoints(settings_),{settings_.feedbackPort,{}},settings_.targets,trace,timing,loopLog,
        [this](Frame f){output_.card(f);},[this](auto t,const auto& f,bool startup){output_.sync(t,f,startup);}),
    control_(settings_.acquisition.cards,[this](const Command& cmd,const auto& cards){
        return socket_.send(cmd,cards,[this](const Command& bytes,const ControlSocket::SendResult& sent){
            if(bytes[4]==3&&bytes[57]==1)
                receiver_.afterStartSend(sent.card,sent.bytes==int(bytes.size())&&sent.error==0);
            if(!trace_)return;TraceRecord r;r.monotonicNs=SocketReceiver::now();r.session=session_;r.stage=6;
            r.reason=bytes[4];r.packet=bytes[57];r.threadId=GetCurrentThreadId();
            r.card=sent.card;r.sourceIPv4=sent.targetIPv4;r.localPort=socket_.localPort();
            r.sourcePort=settings_.controlPort;r.value=sent.error;r.length=sent.bytes<0?0:sent.bytes;
            trace_->push(r);}, [this](const Command& bytes,int card){
        if(bytes[4]==3&&bytes[57]==1)receiver_.beforeStartSend(card);
    });
    },SocketReceiver::now){
    if(settings_.targets.size()!=std::size_t(settings_.acquisition.cards))throw std::invalid_argument("target/card mapping");
    receiver_.feedbackSink=[this](int card,int type,std::int64_t receivedNs){if(feedbackSink)feedbackSink(card,type,receivedNs);};
}
Backend::~Backend(){stop();}
bool Backend::listen(std::string& error){
    if(listening_)return true;
    if(!socket_.open(settings_.localIp,settings_.targets,settings_.controlPort,error))return false;
    try{output_.start();if(!receiver_.start(error)){output_.stop();socket_.close();return false;}}
    catch(const std::exception& e){error=e.what();stop();return false;}
    listening_=true;control_.setListening(true);return true;
}
void Backend::requestStop(){
    receiver_.prepareStop();receiver_.requestStop();output_.requestStop();
}
void Backend::stop(){
    requestStop();receiver_.stop();if(listening_)receiver_.observeShutdown();output_.stop();socket_.close();
    control_.setListening(false);listening_=false;
}
bool Backend::configure(int ns,int a,int b){return control_.configure(configCommand(ns,a,b),SocketReceiver::now());}
bool Backend::startMeasurement(std::uint64_t session){
    bool fenceOk=true;
    const bool sent=control_.start([&]{session_=session;receiver_.prepareStart(session);output_.beginSession(session);},
                                   [&](bool ok){fenceOk=receiver_.completeStart(ok);});
    return sent&&fenceOk;
}
bool Backend::stopMeasurement(){
    return control_.stop([&]{receiver_.prepareStop();},[&](bool ok){receiver_.completeStop(ok);});
}
}
