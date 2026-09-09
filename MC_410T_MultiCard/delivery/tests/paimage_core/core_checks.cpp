#include "PaimageAcquisition/SourceCore.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace paimage;
constexpr Time ms=1000000;
void check(bool ok,const char* label){if(!ok)throw std::runtime_error(label);}
std::vector<std::uint8_t> packet(int seq,int trigger,int bytes=1440){
    std::vector<std::uint8_t> p(bytes+4);p[0]=seq&255;p[1]=(seq>>8)&255;p[2]=trigger&255;p[3]=(trigger>>8)&255;
    for(int i=0;i+8<=bytes;i+=8){std::int32_t b=-10000-seq*180-i/8,a=20000+seq*180+i/8;std::memcpy(p.data()+4+i,&b,4);std::memcpy(p.data()+8+i,&a,4);}return p;
}
struct Harness {
    std::vector<Frame> output;std::vector<std::uint16_t> sync;std::vector<Observation> events;
    Config cfg;SourceCore core;
    Harness(Config c):cfg(c),core(c,[this](Frame f){output.push_back(f);},[this](std::uint16_t t,const auto&,bool){sync.push_back(t);},[this](const Observation& o){events.push_back(o);}){core.prepareStart();core.completeStart(true);}
    void trigger(int t,Time at,int missing=-1,int onlyCard=-1){int size=cfg.samples*(cfg.bits==32?8:4),n=(size+1439)/1440;
        for(int c=0;c<cfg.cards;++c)if(onlyCard<0||c==onlyCard)for(int i=0;i<n;++i)if(i!=missing){auto p=packet(i,t,std::min(1440,size-i*1440));core.ingest(c,p.data(),p.size(),at+i);}}
};
int main(){try{
    for(int samples:{5000,12500}){Harness h({4,samples,32,0});h.trigger(65535,ms);h.trigger(0,26*ms);check(h.output.size()==8&&h.sync.size()==2,"four-card complete and wrap");
        for(const auto& f:h.output){check(f->complete&&f->expected==unsigned(samples==5000?28:70),"packet count");std::vector<float>a,b;decodeRaw(*f,32,a,b);check(a.size()==unsigned(samples)&&a.back()==float(20000+samples-1)&&b.back()==float(-10000-samples+1),"B/A raw numerical contract");}
        auto p=packet(0,0);check(h.core.ingest(0,p.data(),p.size(),27*ms)==Decision::RecentTrigger,"recent completed dedup");}
    {Harness h({4,5000,32,0});h.trigger(1,ms,10);h.core.poll(102*ms);check(h.output.size()==4&&h.sync.empty(),"partial does not synchronize");
        for(auto f:h.output){check(!f->complete&&f->unique==27&&!(f->seen[0]&(1u<<10)),"missing-middle metadata");check(std::all_of(f->bytes.begin()+14400,f->bytes.begin()+15840,[](auto x){return x==0;}),"missing-middle zero fill");}
        check(h.core.counters().runtimeIncomplete==4,"runtime partial count");}
    {Harness h({1,5000,32,0});auto p=packet(1,1);h.core.ingest(0,p.data(),p.size(),ms);check(h.core.ingest(0,p.data(),p.size(),2*ms)==Decision::Duplicate,"duplicate");p=packet(0,1);check(h.core.ingest(0,p.data(),p.size(),3*ms)==Decision::OffsetOutside,"first-arrival anchor preserved");h.core.poll(103*ms);check(h.output.size()==1&&h.output[0]->unique==1,"out-of-range timeout");}
    {Harness h({4,5000,32,1000});for(int t=0;t<20;++t)h.trigger(t,ms+t*25*ms);check(h.output.empty()&&!h.core.confirmed(),"startup 475ms remains buffered");h.trigger(20,501*ms);check(h.core.confirmed()&&h.output.size()==84&&h.sync.size()==21,"startup confirmation releases all buffered output");}
    {Harness h({4,5000,32,1000});h.trigger(1,ms);h.core.poll(1002*ms);check(h.output.empty()&&h.core.counters().startupFilteredCards==4&&h.core.counters().startupFilteredSync==1,"idle filter units");h.trigger(1,1003*ms);check(h.core.startupCardCount()==0,"idle clear preserves recent dedup window");}
    {Harness h({4,5000,32,1000});h.trigger(1,ms,7);h.core.poll(102*ms);check(h.output.empty()&&h.core.counters().startupIncomplete==4,"startup partial is not complete");h.core.prepareStop();h.core.completeStop(true,103*ms);check(h.core.counters().stopBufferedCards==4,"stop buffered partial count");}
    {Harness h({1,5000,32,0});for(int i=0;i<100;++i){h.core.prepareStart();auto p=packet(0,i);check(h.core.ingest(0,p.data(),p.size(),ms)==Decision::Disabled,"start gap disabled");h.core.completeStart(false);check(!h.core.enabled(),"failed start stays disabled");h.core.completeStart(true);h.trigger(i,ms);h.core.prepareStop();h.core.completeStop(true,2*ms);}check(h.output.size()==100,"100 starts single delivery");}
    {Harness h({1,5000,32,0});auto p=packet(0,1);h.core.ingest(0,p.data(),p.size(),ms);h.core.prepareStop();h.core.completeStop(false,2*ms);check(h.core.enabled(),"failed stop restores admission");h.core.prepareStop();h.core.completeStop(true,3*ms);check(h.output.empty()&&h.core.counters().stopActiveCards==1,"stop truncation not silently flushed");}
    {Harness h({4,1,32,1000});for(int i=0;i<=4096;++i)h.trigger(i,ms+i, -1,0);check(!h.core.enabled(),"4096 startup buffer cap pauses source");check(h.core.counters().startupFilteredCards==4096,"overflow filtered cards");}
    {auto c=configCommand(20000,1000,2000);check(c[4]==2&&c[7]==1&&c[8]==244&&c[10]==19&&c[11]==136&&c[14]==250,"CONFIG golden bytes");auto s=startCommand(),e=stopCommand();check(s[0]==250&&s[4]==3&&s[10]==8&&s[57]==1&&e[57]==0,"START STOP golden bytes");check(feedbackType(c.data(),18)==1&&feedbackType(c.data(),60)==2&&feedbackType(c.data(),58)==0,"source feedback classifier");}
    {Harness h({4,1,32,0});
        const std::vector<int> keys={0,64,128,192,8,16,24,32,40};
        const std::vector<std::vector<int>> golden={{32,24,16,8,0,64,128,192,40},{192,128,64,0,8,16,24,32,40}};
        for(int pass=0;pass<2;++pass){
            h.core.prepareStart();h.core.completeStart(true);h.events.clear();
            for(int key:keys)h.trigger(key,ms,-1,0);
            h.trigger(1000,252*ms,-1,0);std::vector<int> expired;
            for(const auto& event:h.events)if(event.decision==Decision::SyncExpired)expired.push_back(event.trigger);
            check(expired==golden[pass],"source hash collision, rehash and retained bucket count after clear");
        }
    }
    std::cout<<"PASS: source recovery component checks (not product integration or EXE differential testing)\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
