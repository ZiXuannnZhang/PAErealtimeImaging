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
    std::cout<<"PASS concurrent shared conversion, 28/70 packet sample counts and raw B/A contract\n";
}
