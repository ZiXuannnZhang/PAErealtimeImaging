#include "PaimageAcquisition/FrameConverter.h"
#include <cstring>
#include <thread>
#include <stdexcept>
#include <iostream>
using namespace paimage;
int main(){
    for(int samples:{5000,12500}){
        FrameConverter converter(32,1000,1000000);
        auto f=std::make_shared<CardFrame>();f->card=2;f->trigger=65535;
        f->first=2000000;f->sourceIPv4=0x0100007f;f->complete=false;
        f->bytes.resize(samples*8);
        for(int i=0;i<samples;++i){int b=-10000-i,a=20000+i;
            std::memcpy(f->bytes.data()+i*8,&b,4);std::memcpy(f->bytes.data()+i*8+4,&a,4);}
        TriggerGroupPtr a,b;
        converter.tagSaveSession(f,7);
        std::thread saving([&]{a=converter.convert(f);});
        std::thread sync([&]{b=converter.convert(f);});saving.join();sync.join();
        if(a!=b||converter.conversions()!=1||a->sampleCount!=samples||a->isComplete||a->sessionGen!=7||
           a->cardId!=2||a->triggerSeq!=65535||a->timestamp_ms!=1001||a->sourceIPv4!=f->sourceIPv4)
            throw std::runtime_error("conversion identity/metadata");
        for(int i=0;i<samples;++i)if(a->freqA[i]!=float(20000+i)||a->freqB[i]!=float(-10000-i))
            throw std::runtime_error("raw B/A numerical contract");
    }
    // Round-terminal metadata: the stable isFinalLogicalTrigger data property
    // must reach the TriggerGroup on first and cached tags alike, while the
    // one-shot roundComplete merge keeps the first true.
    {
        FrameConverter converter(32,1000,1000000);
        auto f=std::make_shared<CardFrame>();f->card=1;f->trigger=42;
        f->first=2000000;f->complete=true;f->bytes.resize(8);f->measurementSession=5;
        PhysicalRoundClassification first;
        first.decision=PhysicalTriggerDecision::LogicalScan;first.measurementSession=5;
        first.roundGeneration=3;first.triggerSeq=42;first.logicalTriggerIndex=10;
        first.newDistinct=true;first.roundComplete=true;first.isFinalLogicalTrigger=true;
        converter.tagNormalization(f,first);
        PhysicalRoundClassification cached=first;
        cached.newDistinct=false;cached.roundComplete=false;   // cached-hit shape
        converter.tagNormalization(f,cached);
        auto g=converter.convert(f);
        if(!g->normalizationApplied||!g->isFinalLogicalTrigger||!g->roundComplete||
           g->roundGeneration!=3||g->logicalTriggerIndex!=10)
            throw std::runtime_error("final trigger terminal metadata propagation");
        if(g->physicalRoundIdentity()!=RoundIdentity{5,3})
            throw std::runtime_error("final trigger round identity propagation");
    }
    // A non-final trigger carries neither marker through the same path.
    {
        FrameConverter converter(32,1000,1000000);
        auto f=std::make_shared<CardFrame>();f->card=0;f->trigger=7;
        f->first=1500000;f->complete=true;f->bytes.resize(8);
        PhysicalRoundClassification mid;
        mid.decision=PhysicalTriggerDecision::LogicalScan;mid.measurementSession=5;
        mid.roundGeneration=3;mid.triggerSeq=7;mid.logicalTriggerIndex=4;mid.newDistinct=true;
        converter.tagNormalization(f,mid);
        auto g=converter.convert(f);
        if(!g->normalizationApplied||g->roundComplete||g->isFinalLogicalTrigger)
            throw std::runtime_error("non-final trigger must not carry terminal metadata");
    }
    std::cout<<"PASS concurrent shared conversion, 28/70 packet sample counts, raw B/A contract and round-terminal metadata\n";
}
