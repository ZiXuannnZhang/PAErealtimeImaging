#include "PaimageAcquisition/FrameConverter.h"
#include <optional>
#include <stdexcept>
namespace paimage {
void FrameConverter::tagSaveSession(Frame frame,std::uint64_t generation){
    if(!generation)return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry=entries_[frame.get()];
    if(!entry||entry->source.lock()!=frame){entry=std::make_shared<Entry>();entry->source=frame;
        if(++inserted_%64==0)for(auto p=entries_.begin();p!=entries_.end();){
            if(p->second->source.expired())p=entries_.erase(p);else ++p;}
    }
    entry->saveSession=generation;
}
void FrameConverter::tagNormalization(Frame frame,const PhysicalRoundClassification& classification){
    if(!frame)return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry=entries_[frame.get()];
    if(!entry||entry->source.lock()!=frame){entry=std::make_shared<Entry>();entry->source=frame;
        if(++inserted_%64==0)for(auto p=entries_.begin();p!=entries_.end();){
            if(p->second->source.expired())p=entries_.erase(p);else ++p;}
    }
    // The sync path classifies the same source frame after the card/save path
    // has already seen it. A cached late-card decision deliberately carries
    // roundComplete=false, but it must not erase the one-shot boundary marker
    // already attached to that frame. isFinalLogicalTrigger is stable across
    // first/cached classifications of the same identity, so it needs no merge.
    if (entry->normalization && entry->normalization->roundComplete &&
        !classification.roundComplete) {
        auto merged = classification;
        merged.roundComplete = true;
        entry->normalization = std::move(merged);
    } else {
        entry->normalization=classification;
    }
}
TriggerGroupPtr FrameConverter::convert(Frame frame){
    if(!frame)throw std::invalid_argument("null source frame");
    std::shared_ptr<Entry> entry;
    {std::lock_guard<std::mutex> lock(mutex_);
        auto it=entries_.find(frame.get());
        if(it!=entries_.end()&&it->second->source.lock()==frame)entry=it->second;
        else{
            entry=std::make_shared<Entry>();entry->source=frame;entries_[frame.get()]=entry;
            if(++inserted_%64==0)for(auto p=entries_.begin();p!=entries_.end();){
                if(p->second->source.expired())p=entries_.erase(p);else ++p;}
        }
    }
    std::call_once(entry->once,[&]{
        auto group=std::make_shared<TriggerGroup>();group->cardId=frame->card;group->triggerSeq=frame->trigger;
        group->sessionGen=entry->saveSession;
        group->measurementSession=frame->measurementSession;
        group->isComplete=frame->complete;group->sourceIPv4=frame->sourceIPv4;
        group->sourceTimedOut=frame->reason==Decision::Timeout;
        if(entry->normalization){
            const auto& n=*entry->normalization;
            group->normalizationApplied=true;
            group->physicalDecision=n.decision;
            group->roundGeneration=n.roundGeneration;
            group->logicalTriggerIndex=n.logicalTriggerIndex;
            group->roundComplete=n.roundComplete;
            group->isFinalLogicalTrigger=n.isFinalLogicalTrigger;
        }
        auto ms=std::int64_t(wallMs_)+(frame->first-monotonic_)/1000000;
        group->timestamp_ms=ms>0?std::uint64_t(ms):0;
        decodeRaw(*frame,bits_,group->freqA,group->freqB);group->sampleCount=int(group->freqA.size());
        entry->group=std::move(group);++conversions_;
    });
    return entry->group;
}
}
