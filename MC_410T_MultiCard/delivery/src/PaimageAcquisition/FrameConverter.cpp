#include "PaimageAcquisition/FrameConverter.h"
#include <algorithm>
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
        group->identity.measurementSession=frame->measurementSession;
        group->identity.wireTrigger=frame->trigger;
        group->identity.cardId=frame->card;
        group->identity.ingressId=frame->firstIngressId;
        group->identity.firstReceiveMonotonicNs=static_cast<std::uint64_t>(std::max<Time>(0, frame->first));
        group->quality.schemaVersion=1;
        group->quality.qualityUnknown=false;
        group->quality.assemblyComplete=frame->complete;
        group->quality.expectedPacketCount=frame->expected;
        group->quality.receivedPacketCount=frame->unique;
        group->quality.expectedPayloadBytes=frame->expectedPayloadBytes;
        group->quality.packetCoverage.assign(frame->expected, 0);
        std::uint64_t actualBytes=0;
        bool coverageComplete=frame->expected>0 && frame->seen.size() >= (frame->expected+31u)/32u;
        bool lengthsValid=frame->expected>0 && frame->lengths.size() >= frame->expected;
        for (std::uint32_t i=0; i<frame->expected; ++i) {
            const bool seen=(i/32u<frame->seen.size()) &&
                ((frame->seen[i/32u]&(1u<<(i%32u)))!=0);
            const std::uint64_t offset=std::uint64_t(i)*1440u;
            const std::uint64_t remaining=frame->expectedPayloadBytes>offset
                ? frame->expectedPayloadBytes-offset : 0;
            const std::uint64_t expectedLength=std::min<std::uint64_t>(1440u, remaining);
            const std::uint64_t length=i<frame->lengths.size()?frame->lengths[i]:0;
            if (seen && i<group->quality.packetCoverage.size())
                group->quality.packetCoverage[i]=1;
            if (!seen) coverageComplete=false;
            actualBytes+=length;
            if (!seen || expectedLength==0 || length!=expectedLength) lengthsValid=false;
        }
        group->quality.actualPayloadBytes=actualBytes;
        group->quality.packetCoverageComplete=coverageComplete;
        group->quality.packetLengthValid=lengthsValid;
        auto ms=std::int64_t(wallMs_)+(frame->first-monotonic_)/1000000;
        group->timestamp_ms=ms>0?std::uint64_t(ms):0;
        decodeRaw(*frame,bits_,group->freqA,group->freqB);group->sampleCount=int(group->freqA.size());
        const std::uint64_t pairBytes=bits_==32?8u:4u;
        group->quality.sampleLengthValid=group->quality.packetCoverageComplete && lengthsValid &&
            actualBytes==frame->expectedPayloadBytes &&
            group->freqA.size()==frame->expectedPayloadBytes/pairBytes;
        group->quality.sampleOriginKnown=false;
        if (!group->quality.packetCoverageComplete) group->quality.missingReason="packet-coverage-incomplete";
        else if (!lengthsValid) group->quality.missingReason="packet-length-invalid";
        else if (!group->quality.sampleLengthValid) group->quality.missingReason="sample-length-invalid";
        entry->group=std::move(group);++conversions_;
    });
    return entry->group;
}
}
