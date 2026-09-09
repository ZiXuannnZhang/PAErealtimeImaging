#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "PaimageAcquisition/HostOutput.h"
#include "PaimageAcquisition/SocketReceiver.h"
#include <QDateTime>
#include <stdexcept>
namespace paimage {
HostOutput::HostOutput(int bits,int block,std::vector<DataProcessor*> processors,
    std::vector<FileSaver*> savers,TraceWriter* trace)
    :processors_(std::move(processors)),savers_(std::move(savers)),trace_(trace),
     converter_(bits,QDateTime::currentMSecsSinceEpoch(),SocketReceiver::now()),
     workers_(int(processors_.size()),block,[this](Frame f){consumeCard(f);},
        [this](const SyncFrame& f){consumeSync(f);},
        [this](auto r,Frame f){observe(f,5,std::uint8_t(r));}) {
    if(processors_.size()!=savers_.size())throw std::invalid_argument("host card mapping");
    for(std::size_t i=0;i<processors_.size();++i){
        auto saver=savers_[i];processors_[i]->setDirectSaveSink([saver](const TriggerGroupPtr& f){return saver->consumeTriggerGroup(f);});
    }
    workers_.setSavingHooks([this]{for(auto s:savers_)s->serviceCloseRequest();},
                           [this]{for(auto s:savers_){if(configurationRestart_)s->suspendForSourceRestart();else s->stopSaving();}});
}
HostOutput::~HostOutput(){stop();}
void HostOutput::observe(Frame f,std::uint8_t stage,std::uint8_t reason,std::uint32_t value){
    if(!trace_)return;TraceRecord r;r.monotonicNs=SocketReceiver::now();
    r.threadId=GetCurrentThreadId();r.stage=stage;r.reason=reason;r.value=value;
    if(f){r.session=f->measurementSession;r.correlation=f->firstIngressId;
        r.card=f->card;r.trigger=f->trigger;r.packet=f->basePacket;r.sourceIPv4=f->sourceIPv4;}
    trace_->push(r);
}
void HostOutput::consumeCard(Frame f){
    auto group=converter_.convert(f);
    auto result=processors_.at(f->card)->deliverAssembled(group,true,false);
    // stage 7: 0 save consumer result, 1 display returned, 2 Ring returned,
    // 3 publisher returned, 4 exception, 5 session stale after conversion.
    observe(f,7,0,std::uint32_t(result.save));
    if(result.exception)observe(f,7,4);
}
void HostOutput::consumeSync(const SyncFrame& sync){
    std::vector<TriggerGroupPtr> groups;groups.reserve(sync.cards.size());
    for(auto f:sync.cards)groups.push_back(converter_.convert(f));
    if(!workers_.isCurrentSession(sync.session)){for(auto f:sync.cards)observe(f,7,5);return;}
    for(std::size_t i=0;i<groups.size();++i){auto f=sync.cards[i];
        auto result=processors_.at(f->card)->deliverAssembled(groups[i],false,true);
        observe(f,7,1,result.display);observe(f,7,2,result.ring);observe(f,7,3,result.publisher);
        if(result.exception)observe(f,7,4);
    }
}
std::uint64_t HostOutput::startSaving(const QString& dir,int count,const QString& suffix){
    return workers_.configureSaving(true,[this,dir,count,suffix]{for(auto s:savers_)s->startSaving(dir,count,suffix);});
}
std::uint64_t HostOutput::stopSaving(){
    return workers_.configureSaving(false,[this]{for(auto s:savers_)s->stopSaving();});
}
std::uint64_t HostOutput::resumeSaving(){
    return workers_.configureSaving(true,[this]{for(auto s:savers_)s->resumeAfterSourceRestart();});
}
void HostOutput::requestClose(){for(auto s:savers_)s->requestClose();}
}
