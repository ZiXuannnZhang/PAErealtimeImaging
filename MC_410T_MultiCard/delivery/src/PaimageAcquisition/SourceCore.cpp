#include "PaimageAcquisition/SourceCore.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>
namespace paimage {
namespace { constexpr Time ms=1000000; constexpr std::size_t payload=1440;
std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0]|(unsigned(p[1])<<8)); }
void be(std::uint8_t* p,std::uint32_t v,int n) { while(n) {p[--n]=std::uint8_t(v);v>>=8;} }
std::uint64_t sourceHash(std::uint16_t key){auto h=std::uint64_t(0xcbf29ce484222325);h=(h^(key&255))*0x100000001b3;return (h^(key>>8))*0x100000001b3;}
}
SourceCore::SourceCore(Config c,CardSink out,SyncSink sync,Observer observe)
 :config_(c),cardSink_(std::move(out)),syncSink_(std::move(sync)),observer_(std::move(observe)) {
    if(c.cards<1||c.cards>32||c.samples<1||c.samples>1000000||(c.bits!=16&&c.bits!=32))
        throw std::invalid_argument("unsupported acquisition configuration");
    bytes_=std::size_t(c.samples)*(c.bits==32?8:4); expected_=int((bytes_+payload-1)/payload);
    cards_.resize(c.cards);
    for(auto& a:cards_) {a.bytes.resize(std::size_t(expected_)*payload);
        a.lengths.resize(expected_);a.seen.resize((expected_+31)/32);}
}
void SourceCore::event(Decision d,int c,std::uint16_t t,std::uint16_t p,std::uint32_t n,Time at,std::uint64_t firstIngressId) {
    if(observer_) observer_({d,c,t,p,n,at,firstIngressId});
}
void SourceCore::clearAssembly(Assembly& a,bool recent) {
    a.active=false;a.trigger=a.base=0;a.actual=a.unique=0;a.first=a.last=0;
    a.firstIngressId=0;
    std::fill(a.seen.begin(),a.seen.end(),0);std::fill(a.lengths.begin(),a.lengths.end(),0);
    if(recent)a.recent.clear();
}
void SourceCore::prepareStart(std::uint64_t diagnosticSession,Time now) {
    for(const auto& p:pending_)for(const auto& f:p.cards)if(f)
        event(Decision::StartPendingSyncDiscard,f->card,f->trigger,0,f->unique,now,f->firstIngressId);
    for(int c=0;c<config_.cards;++c)if(cards_[c].active)
        event(Decision::StartActiveDiscard,c,cards_[c].trigger,0,cards_[c].unique,now,cards_[c].firstIngressId);
    diagnosticSession_=diagnosticSession;
    enabled_=false;pending_.clear();for(auto& a:cards_)clearAssembly(a,true);
    confirmed_=config_.startupIdleMs<=0;startupCards_.clear();startupSync_.clear();
    startupBytes_=0;lastStartup_=firstSync_=0;
}
void SourceCore::completeStart(bool success, Time now) {
    if(!success)return;
    // Evidence for the cleanup that completeStart performs: the decision
    // events below cover assemblies and sync candidates discarded here.
    for(const auto& p:pending_)for(const auto& f:p.cards)if(f)
        event(Decision::CompleteStartPendingSyncDiscard,f->card,f->trigger,0,f->unique,now,f->firstIngressId);
    for(int c=0;c<config_.cards;++c)if(cards_[c].active)
        event(Decision::CompleteStartActiveDiscard,c,cards_[c].trigger,0,cards_[c].unique,now,cards_[c].firstIngressId);
    pending_.clear();for(auto& a:cards_)clearAssembly(a,true);enabled_=true;
}
void SourceCore::prepareStop(){enabled_=false;}
void SourceCore::completeStop(bool success,Time now) {
    if(!success){enabled_=true;return;}
    // Observation only: source Stop does not flush active assemblies.
    for(int c=0;c<config_.cards;++c)if(cards_[c].active){++counts_.stopActiveCards;
        event(Decision::StopTruncated,c,cards_[c].trigger,0,cards_[c].unique,now,cards_[c].firstIngressId);}
    counts_.stopBufferedCards+=startupCards_.size();counts_.stopBufferedSync+=startupSync_.size();
    for(const auto& f:startupCards_)event(Decision::StopBufferedDiscard,f->card,f->trigger,0,f->unique,now,f->firstIngressId);
    confirmed_=true;startupCards_.clear();startupSync_.clear();startupBytes_=0;lastStartup_=firstSync_=0;
}
void SourceCore::observeShutdown(Time now){
    for(const auto& p:pending_)for(const auto& f:p.cards)if(f)
        event(Decision::ListenerPendingSyncDiscard,f->card,f->trigger,0,f->unique,now,f->firstIngressId);
    for(int c=0;c<config_.cards;++c)if(cards_[c].active)
        event(Decision::ListenerActiveDiscard,c,cards_[c].trigger,0,cards_[c].unique,now,cards_[c].firstIngressId);
    for(const auto& f:startupCards_)event(Decision::ListenerBufferedDiscard,f->card,f->trigger,0,f->unique,now,f->firstIngressId);
}
void SourceCore::clearStartup(Time now,bool count,Decision reason) {
    for(const auto& p:pending_)for(const auto& f:p.cards)if(f)
        event(Decision::StartupPendingSyncDiscard,f->card,f->trigger,0,f->unique,now,f->firstIngressId);
    for(int c=0;c<config_.cards;++c)if(cards_[c].active)
        event(Decision::StartupActiveDiscard,c,cards_[c].trigger,0,cards_[c].unique,now,cards_[c].firstIngressId);
    if(count){counts_.startupFilteredCards+=startupCards_.size();counts_.startupFilteredSync+=startupSync_.size();}
    for(const auto& f:startupCards_)event(reason,f->card,f->trigger,0,f->unique,now,f->firstIngressId);
    startupCards_.clear();startupSync_.clear();startupBytes_=0;lastStartup_=firstSync_=0;
    pending_.clear();for(auto& a:cards_)clearAssembly(a);
}
Decision SourceCore::ingest(int c,const std::uint8_t* p,std::size_t n,Time now,std::uint64_t ingressId,std::uint32_t sourceIPv4) {
    if(c<0||c>=config_.cards){event(Decision::InvalidCard,c,0,0,1,now);return Decision::InvalidCard;}
    if(n<4){event(Decision::Short,c,0,0,1,now);return Decision::Short;}
    const auto packet=le16(p),trigger=le16(p+2);
    auto reject=[&](Decision d){event(d,c,trigger,packet,1,now);return d;};
    if(!enabled_)return reject(Decision::Disabled);
    if(!confirmed_&&lastStartup_&&now-lastStartup_>=Time(config_.startupIdleMs)*ms)clearStartup(now,true);
    auto& a=cards_[c];
    if(std::find(a.recent.begin(),a.recent.end(),trigger)!=a.recent.end())return reject(Decision::RecentTrigger);
    if(!confirmed_)lastStartup_=now;
    if(a.active&&a.trigger!=trigger){close(c,Decision::TriggerSwitch,now);clearAssembly(a);if(!enabled_)return reject(Decision::Disabled);}
    if(!a.active){a.active=true;a.trigger=trigger;a.base=packet;a.first=now;a.firstIngressId=ingressId;a.sourceIPv4=sourceIPv4;}
    ++a.actual;a.last=now;
    const auto slot=std::uint16_t(packet-a.base);
    Decision result=Decision::Accepted;
    if(slot>=expected_)result=Decision::OffsetOutside;
    else if(a.seen[slot/32]&(1u<<(slot%32)))result=Decision::Duplicate;
    else {a.seen[slot/32]|=1u<<(slot%32);const auto length=std::min(n-4,payload);
        std::memcpy(a.bytes.data()+slot*payload,p+4,length);a.lengths[slot]=std::uint16_t(length);++a.unique;}
    event(result,c,trigger,packet,1,now);
    if(a.unique>=unsigned(expected_)){close(c,Decision::Complete,now);clearAssembly(a);}
    return result;
}
void SourceCore::poll(Time now){
    if(!enabled_)return;
    if(!confirmed_&&lastStartup_&&now-lastStartup_>=Time(config_.startupIdleMs)*ms)clearStartup(now,true);
    for(int c=0;c<config_.cards;++c){auto& a=cards_[c];
        if(a.active&&a.unique&&a.last&&now-a.last>=100*ms){close(c,Decision::Timeout,now);clearAssembly(a);if(!enabled_)break;}}
}
void SourceCore::deliverCard(Frame f,Time now){event(Decision::CardOutput,f->card,f->trigger,0,f->unique,now,f->firstIngressId);if(cardSink_)cardSink_(std::move(f));}
void SourceCore::deliverSync(std::uint16_t seq,std::vector<Frame> frames,Time now){
    // 0x1401342e0: count>=4 AND elapsed>=500ms; no consecutive-seq test.
    if(config_.startupIdleMs>0&&!confirmed_){
        if(startupSync_.empty())firstSync_=now;
        startupSync_.emplace_back(seq,std::move(frames));
        if(startupSync_.size()<4||now-firstSync_<500*ms)return;
        confirmed_=true;event(Decision::StartupConfirmed,-1,seq,0,unsigned(startupSync_.size()),now);
        for(auto& f:startupCards_)deliverCard(f,now);
        startupCards_.clear();startupBytes_=0;lastStartup_=0;
        for(auto& s:startupSync_){event(Decision::SyncOutput,-1,s.first,0,unsigned(s.second.size()),now);if(syncSink_)syncSink_(s.first,s.second,true);}
        startupSync_.clear();firstSync_=0;
    }else{event(Decision::SyncOutput,-1,seq,0,unsigned(frames.size()),now);if(syncSink_)syncSink_(seq,frames,false);}
}
std::list<SourceCore::Pending>::iterator SourceCore::findOrInsertPending(std::uint16_t seq,Time now){
    auto found=std::find_if(pending_.begin(),pending_.end(),[&](const Pending& p){return p.trigger==seq;});
    if(found!=pending_.end())return found;
    // VA 0x14012b9b0, 0x14012f340: FNV-1a, load 1, eightfold growth below 512.
    // Rehash traverses the existing list and moves distinct colliding keys before
    // the first previously processed key in that new bucket.
    if(pending_.size()+1>bucketCount_){
        bucketCount_=bucketCount_<512?bucketCount_*8:bucketCount_*2;
        std::vector<std::list<Pending>::iterator> original;
        for(auto it=pending_.begin();it!=pending_.end();++it)original.push_back(it);
        std::vector<std::list<Pending>::iterator> first(bucketCount_,pending_.end());
        for(auto it:original){auto slot=sourceHash(it->trigger)&(bucketCount_-1);
            if(first[slot]!=pending_.end())pending_.splice(first[slot],pending_,it);
            first[slot]=it;}
    }
    auto slot=sourceHash(seq)&(bucketCount_-1);
    auto before=std::find_if(pending_.begin(),pending_.end(),[&](const Pending& p){return (sourceHash(p.trigger)&(bucketCount_-1))==slot;});
    return pending_.insert(before,Pending{seq,now,std::vector<Frame>(config_.cards)});
}
void SourceCore::close(int c,Decision why,Time now){
    auto& a=cards_[c];a.recent.push_back(a.trigger);if(a.recent.size()>16)a.recent.pop_front();
    auto f=std::make_shared<CardFrame>();f->card=c;f->trigger=a.trigger;f->basePacket=a.base;
    f->actual=a.actual;f->unique=a.unique;f->expected=expected_;f->first=a.first;f->last=a.last;f->closed=now;
    f->measurementSession=diagnosticSession_;f->firstIngressId=a.firstIngressId;
    f->sourceIPv4=a.sourceIPv4;
    f->reason=why;f->complete=a.unique>=unsigned(expected_);f->lengths=a.lengths;f->seen=a.seen;f->bytes.resize(bytes_,0);
    for(int i=0;i<expected_;++i){auto at=std::size_t(i)*payload;auto length=std::min(std::size_t(a.lengths[i]),bytes_-at);
        if(length)std::memcpy(f->bytes.data()+at,a.bytes.data()+at,length);}
    event(why,c,a.trigger,a.base,a.unique,now,a.firstIngressId);
    if(config_.startupIdleMs>0&&!confirmed_){
        if(startupCards_.size()>=4096||f->bytes.size()>128u*1024u*1024u-startupBytes_){
            enabled_=false;event(Decision::StartupOverflow,c,a.trigger,0,a.unique,now);clearStartup(now,true,Decision::StartupOverflowDiscard);return;}
        startupBytes_+=f->bytes.size();startupCards_.push_back(f);event(Decision::StartupBuffered,c,f->trigger,0,f->unique,now,f->firstIngressId);
    }else deliverCard(f,now);
    if(!f->complete){if(confirmed_)++counts_.runtimeIncomplete;else ++counts_.startupIncomplete;return;}
    ++counts_.completeCards;
    auto position=findOrInsertPending(f->trigger,now);auto& entry=*position;
    entry.cards[c]=f;
    if(std::all_of(entry.cards.begin(),entry.cards.end(),[](const Frame& x){return bool(x);})){auto frames=std::move(entry.cards);pending_.erase(position);deliverSync(f->trigger,std::move(frames),now);return;}
    // Source eviction runs here, not periodically on poll().
    for(auto it=pending_.begin();it!=pending_.end();){
        if(now-it->first>250*ms||pending_.size()>16){event(Decision::SyncExpired,-1,it->trigger,0,1,now);
            if(confirmed_)++counts_.runtimeIncomplete;else ++counts_.startupIncomplete;it=pending_.erase(it);
        }else ++it;
    }
}
Command configCommand(std::int32_t ns,std::int32_t a,std::int32_t b){Command out{};std::fill_n(out.begin(),4,0xfa);out[4]=2;
    be(out.data()+5,std::uint32_t(b/4),4);be(out.data()+9,std::uint32_t(ns/4),3);be(out.data()+12,std::uint32_t(a/4),3);return out;}
Command startCommand(){Command out{};std::fill_n(out.begin(),4,0xfa);out[4]=3;out[10]=8;out[57]=1;return out;}
Command stopCommand(){auto out=startCommand();out[57]=0;return out;}
int feedbackType(const std::uint8_t* data,std::size_t n){return !data?0:n==18?1:n==60?2:0;}
void decodeRaw(const CardFrame& f,int bits,std::vector<float>& a,std::vector<float>& b){
    if(bits!=16&&bits!=32)throw std::invalid_argument("sample width");
    const std::size_t pair=bits==32?8:4;a.resize(f.bytes.size()/pair);b.resize(a.size());
    for(std::size_t i=0;i<a.size();++i){if(bits==32){std::int32_t x,y;std::memcpy(&x,f.bytes.data()+i*pair,4);std::memcpy(&y,f.bytes.data()+i*pair+4,4);b[i]=float(x);a[i]=float(y);}
        else {std::int16_t x,y;std::memcpy(&x,f.bytes.data()+i*pair,2);std::memcpy(&y,f.bytes.data()+i*pair+2,2);b[i]=float(x);a[i]=float(y);}}
}
}
