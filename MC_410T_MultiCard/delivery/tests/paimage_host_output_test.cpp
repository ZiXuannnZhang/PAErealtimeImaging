#include "PaimageAcquisition/HostOutput.h"
#include "FrontendPreprocessor.h"
#include "AcqConfig.h"
#include "FramePublisher.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
using namespace paimage;
using namespace std::chrono_literals;
void require(bool b,const char* message="host delivery contract"){
    if(!b)throw std::runtime_error(message);
}
template<class F> bool until(F f){auto end=std::chrono::steady_clock::now()+3s;while(!f()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);return f();}
// Production shape for the per-card Frontend Preprocessing Stage: DisplayBuffer
// and the Ring sink consume the frontend-owned clone on the stage worker.  The
// HostOutput assertions below therefore observe the real delivery path.
//
// The frontend high/low-pass filter is explicitly switched off here: the golden
// assertions below are a payload-routing contract (each card's constant A/B
// amplitude must reach the Ring sink intact, which is what detects a cross-card
// mix-up).  Filter numerics, effective-moment and save isolation are covered by
// frontend_preprocessor_test F1..F13; keeping the filter on here would only
// restate those and would blur the routing fingerprint this test relies on.
std::unique_ptr<FrontendPreprocessor> makeFrontend(int card,DisplayBuffer* display,
        FrontendPreprocessor::RingSink ring){
    auto stage=std::make_unique<FrontendPreprocessor>(card);
    frontend_filter::Config noFilter;
    noFilter.hpEnable=false;noFilter.lpEnable=false;
    require(stage->setFilterConfig(noFilter),"frontend filter paths disabled");
    stage->setDisplayBuffer(display);
    stage->setRingSink(std::move(ring));
    stage->start();
    return stage;
}
// Wire the HostOutput session / TimeoutBoundary facts into the stages exactly as
// NetworkController does in production.
void wireFrontends(HostOutput& output,std::vector<std::unique_ptr<FrontendPreprocessor>>& frontends){
    output.setFrontendSessionSink([&frontends](std::uint64_t session){
        for(auto& stage:frontends){if(session==0)stage->endSession();else stage->beginSession(session);}
    });
    output.setFrontendBarrierSink([&frontends](std::uint64_t session,std::uint64_t generation){
        for(auto& stage:frontends)stage->advanceRoundBarrier(session,generation);
    });
}
void stopFrontends(std::vector<std::unique_ptr<FrontendPreprocessor>>& frontends){
    for(auto& stage:frontends)stage->stop();
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);
    for(int samples:{5000,12500}){
        QTemporaryDir dir(QDir::currentPath()+"/host-output-XXXXXX");require(dir.isValid());
        std::vector<std::unique_ptr<DisplayBuffer>> displays;
        std::vector<std::unique_ptr<FrontendPreprocessor>> frontends;
        std::vector<std::unique_ptr<FileSaver>> savers;
        std::vector<std::unique_ptr<DataProcessor>> processors;
        std::vector<DataProcessor*> pp;std::vector<FileSaver*> ss;
        std::atomic<int> rings{0};std::atomic<bool> values{true};
        AcqConfig cfg;cfg.acqTimeNs=samples*4;
        for(int card=0;card<4;++card){
            displays.push_back(std::make_unique<DisplayBuffer>());savers.push_back(std::make_unique<FileSaver>(card));
            frontends.push_back(makeFrontend(card,displays.back().get(),
                [&](const TriggerGroupConstPtr& frame){
                    if(!frame||frame->triggerSeq!=9||frame->freqA.size()!=std::size_t(samples)||frame->freqB.size()!=frame->freqA.size())values=false;
                    for(std::size_t i=0;i<frame->freqA.size();++i)if(frame->freqA[i]!=frame->cardId+1||frame->freqB[i]!=-frame->cardId-1)values=false;
                    ++rings;return ImagingSubmitResult::Accepted;
                }));
            processors.push_back(std::make_unique<DataProcessor>(card,nullptr,nullptr,cfg,
                [stage=frontends.back().get()](const TriggerGroupPtr& frame){return stage->submit(frame);}));
            pp.push_back(processors.back().get());ss.push_back(savers.back().get());
        }
        HostOutput output(32,50,pp,ss,nullptr);wireFrontends(output,frontends);output.beginSession(1);output.start();
        auto save=output.startSaving(dir.path(),100,"golden");require(until([&]{return output.savingApplied(save);}));
        SourceCore source({4,samples,32,0},[&](Frame f){output.card(f);},
            [&](auto t,const auto& f,bool s){output.sync(t,f,s);});
        source.prepareStart(1);source.completeStart(true,0);
        const int size=samples*8;
        for(int card=0;card<4;++card)for(int offset=0;offset<size;offset+=1440){
            const int n=std::min(1440,size-offset),seq=offset/1440;std::vector<std::uint8_t> p(n+4);
            p[0]=seq;p[2]=9;for(int i=0;i<n;i+=8){int a=card+1,b=-a;std::memcpy(p.data()+4+i,&b,4);std::memcpy(p.data()+8+i,&a,4);}
            source.ingest(card,p.data(),p.size(),1000000+seq,card*100+seq+1,0x0100007f);
        }
        require(until([&]{return rings==4 && savers[0]->savedCount()==1&&savers[1]->savedCount()==1&&savers[2]->savedCount()==1&&savers[3]->savedCount()==1
            &&frontends[0]->snapshot().displayUpdates==1&&frontends[1]->snapshot().displayUpdates==1
            &&frontends[2]->snapshot().displayUpdates==1&&frontends[3]->snapshot().displayUpdates==1;}));
        auto stopped=output.stopSaving();require(until([&]{return output.savingApplied(stopped);}));output.stop();require(values);
        stopFrontends(frontends);
        const std::uint16_t half[]={0x3c00,0x4000,0x4200,0x4400};
        for(int c=0;c<4;++c){require(!processors[c]->isRunning()&&!savers[c]->isRunning()&&savers[c]->queueDepth()==0);
            DisplayBuffer::Snapshot snap;require(displays[c]->tryRead(snap)&&snap.triggerSeq==9&&snap.sampleCount==samples);
            for(auto channel:{QString("A"),QString("B")}){QFile file(dir.filePath(QString("Card%1_Ch%2_golden_000.dat").arg(c+1).arg(channel)));
                require(file.open(QIODevice::ReadOnly));auto bytes=file.readAll();require(bytes.size()==samples*2);
                auto expected=std::uint16_t(half[c]|(channel=="B"?0x8000:0));
                for(int i=0;i<samples;++i){std::uint16_t value;std::memcpy(&value,bytes.constData()+2*i,2);require(value==expected);}
            }
        }
    }
    {
        QTemporaryDir root(QDir::currentPath()+"/save-generation-XXXXXX");require(root.isValid());
        std::atomic<std::uint64_t> generation{1};std::atomic<bool> entered{false},release{false};
        DisplayBuffer display;FileSaver saver(0);AcqConfig config;config.acqTimeNs=20000;
        DataProcessor processor(0,nullptr,nullptr,config,{});
        processor.setSessionGenReader([&]{return generation.load();});
        saver.setSessionDirResolver([&](std::uint64_t g){
            if(g==1){entered=true;auto deadline=std::chrono::steady_clock::now()+3s;
                while(!release&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);}
            return root.path()+QString("/gen%1").arg(g);
        });
        HostOutput output(32,50,{&processor},{&saver},nullptr);output.start();
        auto applied=output.startSaving(root.path(),100,"generation");require(until([&]{return output.savingApplied(applied);}));
        auto make=[](int trigger){auto frame=std::make_shared<CardFrame>();frame->card=0;frame->trigger=trigger;frame->complete=true;frame->bytes.resize(40000);return frame;};
        output.card(make(1));require(until([&]{return entered.load();}));
        output.card(make(2));generation=2;output.card(make(3));release=true;
        require(until([&]{return saver.savedCount()==3;}));
        auto stopped=output.stopSaving();require(until([&]{return output.savingApplied(stopped);}));output.stop();
        for(int g:{1,2})for(auto ch:{QString("A"),QString("B")}){
            QFile file(root.path()+QString("/gen%1/Card1_Ch%2_generation_000.dat").arg(g).arg(ch));
            require(file.open(QIODevice::ReadOnly));require(file.size()==(g==1?20000:10000));
        }
    }
    // T11: the production save and sync/Ring paths consume the same
    // normalization decision. The first visible identity is filtered from
    // both paths, and the N logical identities are identical in saved bytes
    // and Ring metadata.
    {
        QTemporaryDir root(QDir::currentPath()+"/normalized-output-XXXXXX");require(root.isValid());
        DisplayBuffer display;FileSaver saver(0);AcqConfig config;config.acqTimeNs=64;
        std::mutex ringMutex;std::vector<std::uint16_t> ringTriggers;std::vector<std::int64_t> ringIndices;
        std::vector<bool> ringBoundaries;
        std::vector<PhysicalRoundEvent> roundEvents;
        std::vector<std::unique_ptr<FrontendPreprocessor>> frontends;
        frontends.push_back(makeFrontend(0,&display,
            [&](const TriggerGroupConstPtr& frame){
                std::lock_guard<std::mutex> lock(ringMutex);
                ringTriggers.push_back(frame->triggerSeq);
                ringIndices.push_back(frame->logicalTriggerIndex);
                ringBoundaries.push_back(frame->roundComplete);
                return ImagingSubmitResult::Accepted;
            }));
        DataProcessor processor(0,nullptr,nullptr,config,
            [stage=frontends.back().get()](const TriggerGroupPtr& frame){return stage->submit(frame);});
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,3,
            [&](const PhysicalRoundEvent& event){roundEvents.push_back(event);});
        wireFrontends(output,frontends);
        output.beginSession(77);output.start();
        const auto save=output.startSaving(root.path(),100,"normalized");
        require(until([&]{return output.savingApplied(save);}));
        auto make=[&](std::uint16_t trigger){
            auto frame=std::make_shared<CardFrame>();frame->card=0;frame->trigger=trigger;
            frame->measurementSession=77;frame->first=1;frame->complete=true;frame->reason=Decision::Complete;
            frame->bytes.resize(16*8);
            for(int i=0;i<16;++i){const std::int32_t b=-static_cast<std::int32_t>(trigger);
                const std::int32_t a=static_cast<std::int32_t>(trigger);
                std::memcpy(frame->bytes.data()+i*8,&b,4);
                std::memcpy(frame->bytes.data()+i*8+4,&a,4);}
            return frame;
        };
        auto control=make(100);output.card(control);output.sync(100,{control},false);
        for(std::uint16_t trigger=101;trigger<=103;++trigger){
            auto frame=make(trigger);output.card(frame);output.sync(trigger,{frame},false);
        }
        require(until([&]{std::lock_guard<std::mutex> lock(ringMutex);
            return saver.savedCount()==3&&ringTriggers.size()==3;}));
        const auto stopped=output.stopSaving();require(until([&]{return output.savingApplied(stopped);}));
        output.stop();stopFrontends(frontends);
        {
            std::lock_guard<std::mutex> lock(ringMutex);
            require((ringTriggers==std::vector<std::uint16_t>{101,102,103})&&
                        (ringIndices==std::vector<std::int64_t>{0,1,2})&&
                        (ringBoundaries==std::vector<bool>{false,false,true}),
                    "T11 Ring identities");
        }
        require(roundEvents.size()==2&&
                    roundEvents[0].kind==PhysicalRoundEvent::Kind::ControlFiltered&&
                    roundEvents[1].kind==PhysicalRoundEvent::Kind::CountBoundary,
                "T11 round events");
        QFile savedA(root.filePath("Card1_ChA_normalized_000.dat"));
        require(savedA.open(QIODevice::ReadOnly)&&savedA.size()==3*16*2,
                "T11 saved logical count");
        // FileSaver's float16 output is constant per trigger in this fixture;
        // the three chunks therefore prove that the filtered control value
        // 100 never reached the save path.
        const QByteArray bytes=savedA.readAll();
        for (int logical = 0; logical < 3; ++logical) {
            const std::uint16_t expected = static_cast<std::uint16_t>(0x5650 + logical * 0x10);
            for (int sample = 0; sample < 16; ++sample) {
                std::uint16_t actual = 0;
                std::memcpy(&actual, bytes.constData() +
                                      (logical * 16 + sample) * 2, sizeof(actual));
                require(actual == expected, "T11 saved identity");
            }
        }
    }
    // T12 / timeout T1+T7: exercise the production HostOutput card+sync,
    // save and Ring paths with a real monotonic idle boundary. The timeout is
    // detected while the next identity is classified, before that identity
    // can be admitted to either output path.
    {
        QTemporaryDir root(QDir::currentPath()+"/normalized-timeout-XXXXXX");require(root.isValid());
        DisplayBuffer display;FileSaver saver(0);AcqConfig config;config.acqTimeNs=64;
        std::mutex ringMutex;std::vector<std::uint16_t> ringTriggers;std::vector<std::int64_t> ringIndices;
        std::atomic<int> ringCount{0};
        std::vector<bool> ringBoundaries;std::vector<PhysicalRoundEvent> roundEvents;
        std::vector<std::unique_ptr<FrontendPreprocessor>> frontends;
        frontends.push_back(makeFrontend(0,&display,
            [&](const TriggerGroupConstPtr& frame){
                std::lock_guard<std::mutex> lock(ringMutex);
                ringTriggers.push_back(frame->triggerSeq);
                ringIndices.push_back(frame->logicalTriggerIndex);
                ringBoundaries.push_back(frame->roundComplete);
                ++ringCount;
                return ImagingSubmitResult::Accepted;
            }));
        DataProcessor processor(0,nullptr,nullptr,config,
            [stage=frontends.back().get()](const TriggerGroupPtr& frame){return stage->submit(frame);});
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,3,
            [&](const PhysicalRoundEvent& event){roundEvents.push_back(event);},1.0e-6);
        wireFrontends(output,frontends);
        output.beginSession(79);output.start();
        const auto save=output.startSaving(root.path(),100,"timeout");
        require(until([&]{return output.savingApplied(save);}),
                "timeout T1 save configuration");
        auto make=[&](std::uint16_t trigger,std::int64_t time){
            auto frame=std::make_shared<CardFrame>();frame->card=0;frame->trigger=trigger;
            frame->measurementSession=79;frame->first=time;frame->closed=time;
            frame->complete=true;frame->reason=Decision::Complete;frame->bytes.resize(16*8);
            for(int i=0;i<16;++i){const std::int32_t b=-static_cast<std::int32_t>(trigger);
                const std::int32_t a=static_cast<std::int32_t>(trigger);
                std::memcpy(frame->bytes.data()+i*8,&b,4);
                std::memcpy(frame->bytes.data()+i*8+4,&a,4);}
            return frame;
        };
        auto deliver=[&](std::uint16_t trigger,std::int64_t time){
            auto frame=make(trigger,time);output.card(frame);
            output.sync(trigger,{frame},false);
        };
        deliver(100,1000);deliver(101,1500);deliver(102,1800);
        // The truncated round's frames reach Ring before the idle gap is
        // observed: that is the delivery contract this test documents.  A frame
        // still queued when the TimeoutBoundary fires is stale-dropped by the
        // frontend barrier instead (see frontend_preprocessor_test T9).
        require(until([&]{return ringCount.load()==2;}),
                "timeout T1 partial round dispatched before the boundary");
        deliver(200,20000);deliver(201,20500);deliver(202,20800);deliver(203,20900);
        require(until([&]{return saver.savedCount()==5&&ringCount.load()==5;}),
                "timeout T1/T7 save and Ring delivery");
        const auto stopped=output.stopSaving();require(until([&]{return output.savingApplied(stopped);}));
        output.stop();stopFrontends(frontends);
        {
            std::lock_guard<std::mutex> lock(ringMutex);
            require((ringTriggers==std::vector<std::uint16_t>{101,102,201,202,203})&&
                        (ringIndices==std::vector<std::int64_t>{0,1,0,1,2})&&
                        (ringBoundaries==std::vector<bool>{false,false,false,false,true}),
                    "timeout T1/T7 shared logical output");
        }
        require(roundEvents.size()==4&&
                    roundEvents[0].kind==PhysicalRoundEvent::Kind::ControlFiltered&&
                    roundEvents[1].kind==PhysicalRoundEvent::Kind::TimeoutBoundary&&
                    roundEvents[1].basis=="physical-idle-timeout"&&
                    roundEvents[1].idleDurationNs==18200&&
                    roundEvents[1].lastDistinctTriggerSeq==102&&
                    roundEvents[1].nextVisibleTriggerSeq==200&&
                    roundEvents[2].kind==PhysicalRoundEvent::Kind::ControlFiltered&&
                    roundEvents[3].kind==PhysicalRoundEvent::Kind::CountBoundary,
                "timeout T1 event ordering and diagnostics");
        // Physical-round business boundary (remediation 20260915): the timeout
        // partial round must be sealed into its own file and the next round's
        // logical scans must go to a new file sequence — one file never spans
        // two roundGenerations. Old expectation (5 triggers in _000.dat)
        // encoded the pre-remediation behavior that the field test disproved.
        require(saver.physicalRoundRolloverCount()==1,
                "timeout T7 one physical-round rollover");
        const auto& rollInfo = saver.lastFileRollover();
        require(rollInfo.oldRoundGeneration==0 && rollInfo.newRoundGeneration==1 &&
                    rollInfo.oldFileTriggerCount==2 && rollInfo.manualMode,
                "timeout T7 rollover metadata (old=gen0/2 triggers sealed)");
        QFile savedA(root.filePath("Card1_ChA_timeout_000.dat"));
        require(savedA.open(QIODevice::ReadOnly)&&savedA.size()==2*16*2,
                "timeout T7 partial round sealed at 2");
        QFile savedB(root.filePath("Card1_ChA_timeout_001.dat"));
        require(savedB.open(QIODevice::ReadOnly)&&savedB.size()==3*16*2,
                "timeout T7 next round written to a new file");
    }

    // T13 / timeout T3: SourceCore's 100ms assembly timeout closes only a
    // partial card. It remains a logical output and must not create a
    // physical-round boundary in HostOutput.
    {
        DisplayBuffer display;FileSaver saver(0);AcqConfig config;config.acqTimeNs=400*4;
        std::atomic<int> ringCount{0};std::atomic<int> timedOutRings{0};
        std::vector<PhysicalRoundEvent> roundEvents;
        std::vector<std::unique_ptr<FrontendPreprocessor>> frontends;
        frontends.push_back(makeFrontend(0,&display,
            [&](const TriggerGroupConstPtr& frame){
                if (frame->sourceTimedOut) ++timedOutRings;
                ++ringCount;
                return ImagingSubmitResult::Accepted;
            }));
        DataProcessor processor(0,nullptr,nullptr,config,
            [stage=frontends.back().get()](const TriggerGroupPtr& frame){return stage->submit(frame);});
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,4,
            [&](const PhysicalRoundEvent& event){roundEvents.push_back(event);},5.0);
        wireFrontends(output,frontends);
        output.beginSession(80);output.start();
        std::atomic<bool> sawAssemblyTimeout{false};
        SourceCore source({1,400,32,0},[&](Frame frame){output.card(frame);},
            [&](auto trigger,const auto& frames,bool startup){output.sync(trigger,frames,startup);},
            [&](const Observation& observation){
                if (observation.decision==Decision::Timeout) sawAssemblyTimeout=true;
            });
        std::uint64_t ingress=1;
        auto sendPacket=[&](std::uint16_t trigger,std::uint16_t packet,
                            std::int64_t time){
            const int totalBytes=400*8;const int offset=int(packet)*1440;
            const int payload=std::min(1440,totalBytes-offset);
            std::vector<std::uint8_t> bytes(payload+4);
            bytes[0]=std::uint8_t(packet);bytes[1]=std::uint8_t(packet>>8);
            bytes[2]=std::uint8_t(trigger);bytes[3]=std::uint8_t(trigger>>8);
            for(int i=0;i<payload;i+=8){const std::int32_t b=-1,a=1;
                std::memcpy(bytes.data()+4+i,&b,4);std::memcpy(bytes.data()+8+i,&a,4);}
            source.ingest(0,bytes.data(),bytes.size(),time,ingress++,0x0100007f);
        };
        auto sendComplete=[&](std::uint16_t trigger,std::int64_t time){
            sendPacket(trigger,0,time);sendPacket(trigger,1,time+1);sendPacket(trigger,2,time+2);
        };
        source.prepareStart(80,0);source.completeStart(true,0);
        sendComplete(10,1000000000LL);sendComplete(11,1100000000LL);
        sendPacket(12,0,1200000000LL);
        source.poll(1400000000LL);
        sendComplete(13,1300000000LL);
        // The partial SourceCore frame is deliberately not sent to sync/Ring;
        // the first complete frame is the startup control, leaving two
        // complete logical frames for Ring.
        require(until([&]{return ringCount.load()==2;}),
                "timeout T3 output delivery");
        const auto sourceCounters=source.counters();
        const auto snapshot=output.normalizerSnapshot();
        require(sawAssemblyTimeout.load() && sourceCounters.runtimeIncomplete>=1 &&
                    snapshot.timeoutBoundaryResets==0 &&
                    snapshot.logicalDistinctAccepted==3 &&
                    snapshot.currentLogicalDistinctCount==3 &&
                    timedOutRings.load()==0 && roundEvents.size()==1 &&
                    roundEvents.front().kind==PhysicalRoundEvent::Kind::ControlFiltered,
                "timeout T3 assembly timeout isolation");
        output.stop();stopFrontends(frontends);
    }
    // C1-C3/C8/C13: real card FileSaver and sync/display/Ring paths; the
    // configured cap never limits raw bytes and is shared across both cards.
    for (int startup : {0,7}) for (bool disable : {false,true}) {
        QTemporaryDir root; require(root.isValid());
        AcqConfig config; config.acqTimeNs=64; 
        DisplayBuffer d0,d1; FileSaver s0(0),s1(1);
        std::mutex mutex; std::vector<std::int64_t> indices;
        std::vector<std::uint64_t> generations; std::vector<int> cards;
        int finals=0,counts=0,timeouts=0;
        auto ring=[&](const TriggerGroupConstPtr& group) {
            std::lock_guard<std::mutex> lock(mutex);
            indices.push_back(group->logicalTriggerIndex); generations.push_back(group->roundGeneration);
            cards.push_back(group->cardId);
            if(group->roundComplete)++finals;
            return ImagingSubmitResult::Accepted;
        };
        std::vector<std::unique_ptr<FrontendPreprocessor>> frontends;
        frontends.push_back(makeFrontend(0,&d0,ring));
        frontends.push_back(makeFrontend(1,&d1,ring));
        DataProcessor p0(0,nullptr,nullptr,config,
            [stage=frontends[0].get()](const TriggerGroupPtr& frame){return stage->submit(frame);});
        DataProcessor p1(1,nullptr,nullptr,config,
            [stage=frontends[1].get()](const TriggerGroupPtr& frame){return stage->submit(frame);});
        HostOutput output(32,50,{&p0,&p1},{&s0,&s1},nullptr,nullptr,4,
            [&](const auto& e){
                if(e.kind==PhysicalRoundEvent::Kind::CountBoundary)++counts;
                if(e.kind==PhysicalRoundEvent::Kind::TimeoutBoundary)++timeouts;
            },1.0,startup,disable);
        wireFrontends(output,frontends);
        output.beginSession(900); output.start();
        auto saved=output.startSaving(root.path(),100,"cap");
        require(until([&]{return output.savingApplied(saved);}));
        auto frame=[](int card,int trigger,std::int64_t time) {
            auto f=std::make_shared<CardFrame>();f->card=card;f->trigger=trigger;
            f->measurementSession=900;f->first=time;f->closed=time;
            f->complete=true;f->reason=Decision::Complete;f->bytes.resize(128);return f;
        };
        auto send=[&](int trigger,std::int64_t time) {
            std::vector<Frame> frames{frame(0,trigger,time),frame(1,trigger,time)};
            for(auto f:frames)output.card(f);
            output.sync(trigger,frames,false);
        };
        const int logical=disable?6:4;
        for(int i=0;i<startup+logical;++i)send(100+i,1000000000LL+i);
        require(until([&]{std::lock_guard<std::mutex> lock(mutex);
            return s0.savedCount()==logical&&s1.savedCount()==logical&&indices.size()==8;}),
            "C2 raw >=N saved, only four logical indices per card reach Ring");
        {std::lock_guard<std::mutex> lock(mutex);
            // One FrontendPreprocessor worker per card preserves that card's
            // trigger order; the two card workers interleave freely.  The shared
            // logical index is therefore asserted per card, not as one global
            // interleaved sequence (Task 3.2: bounded per-card FIFO).
            std::vector<std::int64_t> perCard[2];
            for(std::size_t i=0;i<indices.size();++i){
                require(cards[i]>=0&&cards[i]<2,"C3 card attribution");
                require(generations[i]==0,"C3 multicard shared index");
                perCard[cards[i]].push_back(indices[i]);
            }
            for(int c=0;c<2;++c){
                require(perCard[c].size()==4,"C3 four logical indices per card");
                for(int i=0;i<4;++i)require(perCard[c][std::size_t(i)]==i,"C3 per-card shared index order");
            }
            // roundComplete is the frozen one-shot boundary marker: for a
            // multi-card shared trigger exactly one group -- the frame first
            // classified as newDistinct -- carries it. The other card is
            // first classified from the cache with the one-shot suppressed;
            // isFinalLogicalTrigger stays stable on both.
            require(finals==(disable?0:1),"C1/C2 stable final only when count enabled");}
        require(counts==(disable?0:1) && timeouts==0,"C1/C2 boundary policy");
        require(output.normalizerSnapshot().roundGeneration==(disable?0:1),"C2 cap does not advance generation");
        if(disable) {
            require(output.pollPhysicalRoundTimeout(3000000000LL),"C8 active timeout after cap");
            require(timeouts==1,"C8 exactly one timeout");
            // Late old sync must not reopen Ring after reset; raw keeps old stamp.
            output.sync(100+startup,{frame(0,100+startup,3100000000LL),frame(1,100+startup,3100000000LL)},false);
            for(int i=0;i<startup+1;++i)send(200+i,3200000000LL+i);
            require(until([&]{std::lock_guard<std::mutex> lock(mutex);
                return indices.size()==10&&s0.savedCount()==7&&s1.savedCount()==7;}),"C8 new cap opens after startup");
            std::lock_guard<std::mutex> lock(mutex);
            require(indices[8]==0&&indices[9]==0&&generations[8]==1&&generations[9]==1&&finals==0,
                    "C7/C8 old late sync suppressed; new index zero, no synthetic final");
        }
        auto stopped=output.stopSaving();require(until([&]{return output.savingApplied(stopped);}));output.stop();
        stopFrontends(frontends);
        for(int card=1;card<=2;++card) {
            QFile file(root.filePath(QString("Card%1_ChA_cap_000.dat").arg(card)));
            require(file.open(QIODevice::ReadOnly)&&file.size()==logical*16*2,"C2 actual raw bytes beyond cap");
        }
        std::cout<<"PASS C1-C3/C8/C13 startup="<<startup<<" disable="<<disable<<" raw="<<logical<<" realtime=4/card\n";
    }
    // Session A production seam: HostOutput exposes the core policy controls
    // without requiring callers to reach into PhysicalRoundNormalizer.
    {
        DisplayBuffer display;FileSaver saver(0);AcqConfig config;config.acqTimeNs=64;
        DataProcessor processor(0,nullptr,nullptr,config,{});
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,4);
        output.setStartupFilterTriggerCount(7);
        output.setDisableCountBoundary(true);
        output.beginSession(81);
        const auto snapshot = output.normalizerSnapshot();
        require(snapshot.startupFilterTriggerCount == 7 &&
                    snapshot.disableCountBoundary &&
                    snapshot.currentPhysicalDistinctCount == 0 &&
                    snapshot.lastCompletedPhysicalDistinctCount == 0,
                "HostOutput Session A policy seam");
    }
    // Frontend refresh gate: the threshold is the per-round designed trigger
    // count derived from 「单圈总A-line数 / 启用通道数」, never a hard-coded 4000.
    // Path split is what makes the field symptom "display+imaging frozen while
    // saving continues" possible: card() owns raw save, sync() owns the frontend.
    {
        QTemporaryDir root(QDir::currentPath() + "/gate-XXXXXX");
        require(root.isValid(), "gate temp dir");
        FileSaver saver(0);
        AcqConfig config;
        config.acqTimeNs = 64;
        std::atomic<int> frontends{0};
        std::atomic<int> imagingArrivals{0}, frontendOnlyArrivals{0};
        DataProcessor processor(0, nullptr, nullptr, config,
            [&](const TriggerGroupPtr& group) {
                // The count-boundary FrontendOnly stamp is visible at the
                // frontend sink: it is what separates "frontend signal path
                // refresh" from "imaging publisher arrival" below.
                if (group->frontendDisplayOnly) ++frontendOnlyArrivals;
                else ++imagingArrivals;
                ++frontends;
                return FrontendSubmitResult::Accepted;
            });
        // designed triggers/round = 5, no startup filter, count boundary disabled.
        HostOutput output(32, 50, {&processor}, {&saver}, nullptr, nullptr, 5,
                          {}, 0.0, 0, true);
        output.beginSession(9001);
        output.start();
        auto saved = output.startSaving(root.path(), 100, "gate");
        require(until([&] { return output.savingApplied(saved); }), "gate saving applied");
        auto make = [](int trigger) {
            auto f = std::make_shared<CardFrame>();
            f->card = 0;
            f->trigger = trigger;
            f->measurementSession = 9001;
            f->first = 1000000000LL + trigger;
            f->closed = f->first;
            f->complete = true;
            f->reason = Decision::Complete;
            f->bytes.resize(128);
            return f;
        };
        for (int i = 0; i < 12; ++i) {
            auto f = make(100 + i);
            output.card(f);
            output.sync(100 + i, {f}, false);
        }
        require(until([&] { return saver.savedCount() == 12; }),
                "G1 raw save is untouched by the frontend gate");
        require(until([&] { return frontends.load() == 12; }),
                "G1 every trigger keeps reaching the frontend signal path beyond the count boundary");
        require(imagingArrivals.load() == 5 && frontendOnlyArrivals.load() == 7,
                "G1 imaging arrivals stop at the designed five; the rest arrive frontend-only");
        // Live threshold update: the gate must follow immediately, with no
        // round boundary, no session boundary and no service restart.
        output.setConfiguredLogicalTriggersPerRound(20);
        for (int i = 12; i < 16; ++i) {
            auto f = make(100 + i);
            output.card(f);
            output.sync(100 + i, {f}, false);
        }
        require(until([&] { return frontends.load() == 16; }),
                "G2 the frontend signal path continues across the live threshold update");
        require(until([&] { return imagingArrivals.load() == 9; }),
                "G2 imaging arrivals follow the live threshold update");
        require(until([&] { return saver.savedCount() == 16; }),
                "G2 raw save stays complete across the update");
        output.stop();
    }
    // Count-boundary FrontendOnly contract (任务文档
    // 前端刷新闸门解耦_计数边界后时频持续刷新_20260929-023810, 勾选「禁用计数
    // 重置」): triggers at or beyond the designed per-round count keep
    // refreshing the frontend signal windows (displayUpdates increments) while
    // both imaging publishers stay frozen at N -- the FramePublisher submit
    // and the frontend stage's Ring dispatch (the ImagingBypass feed that
    // drives the realtime image). Save continues throughout. The two Drop
    // conditions (startup control trigger, expired-round frame) must reach
    // neither frontend nor publisher in any mode, and the unchecked mode must
    // pass the same input through bit-identically to the pre-change behavior.
    {
        QTemporaryDir root(QDir::currentPath()+"/frontend-only-XXXXXX");
        require(root.isValid(), "frontend-only temp dir");
        FileSaver saver(0);
        AcqConfig config;config.acqTimeNs=64;
        DisplayBuffer display;
        std::atomic<int> ringArrivals{0},ringFinals{0};
        std::vector<std::unique_ptr<FrontendPreprocessor>> frontends;
        frontends.push_back(makeFrontend(0,&display,
            [&](const TriggerGroupConstPtr& frame){
                if(frame&&frame->roundComplete)++ringFinals;
                ++ringArrivals;
                return ImagingSubmitResult::Accepted;
            }));
        FramePublisher publisher;
        publisher.configure(true,1,16);
        std::atomic<int> publisherFrames{0};
        QObject::connect(&publisher,&FramePublisher::framePublished,
            qApp,[&](int,int){++publisherFrames;},Qt::DirectConnection);
        publisher.start();
        DataProcessor processor(0,nullptr,&publisher,config,
            [stage=frontends.back().get()](const TriggerGroupPtr& frame){return stage->submit(frame);});
        // Designed N=4 triggers/round, 「禁用计数重置」 checked, one startup
        // control identity (Drop), and a 1s physical idle timeout so the
        // expired-round Drop can be exercised deterministically.
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,4,
            {},1.0,1,true);
        wireFrontends(output,frontends);
        output.beginSession(9100);output.start();
        const auto save=output.startSaving(root.path(),100,"frontend-only");
        require(until([&]{return output.savingApplied(save);}),
                "FO save configuration");
        auto make=[&](std::uint64_t session,std::uint16_t trigger,std::int64_t time){
            auto frame=std::make_shared<CardFrame>();frame->card=0;frame->trigger=trigger;
            frame->measurementSession=session;frame->first=time;frame->closed=time;
            frame->complete=true;frame->reason=Decision::Complete;frame->bytes.resize(16*8);
            for(int i=0;i<16;++i){const std::int32_t b=-static_cast<std::int32_t>(trigger);
                const std::int32_t a=static_cast<std::int32_t>(trigger);
                std::memcpy(frame->bytes.data()+i*8,&b,4);
                std::memcpy(frame->bytes.data()+i*8+4,&a,4);}
            return frame;
        };
        auto send=[&](std::uint16_t trigger,std::int64_t time){
            auto frame=make(9100,trigger,time);output.card(frame);output.sync(trigger,{frame},false);
        };
        // FO1 Drop invariance (startup control): the first distinct identity
        // is classified OperationalStartupControl and reaches neither the
        // frontend nor any publisher nor save. The gate decision is taken
        // synchronously before any enqueue, so the zero counts are stable.
        send(100,1000000000LL);
        require(frontends[0]->snapshot().displayUpdates==0&&ringArrivals.load()==0&&
                publisherFrames.load()==0&&saver.savedCount()==0,
                "FO1 startup control trigger reaches neither frontend nor publisher");
        // FO2 the N designed logical triggers: display, both imaging
        // publishers and save each receive exactly N.
        for(std::uint16_t trigger=101;trigger<=104;++trigger)
            send(trigger,1000000000LL+trigger);
        require(until([&]{return frontends[0]->snapshot().displayUpdates==4&&
                                  ringArrivals.load()==4&&publisherFrames.load()==4&&
                                  saver.savedCount()==4;}),
                "FO2 N designed triggers reach display, imaging publishers and save");
        // FO3 beyond the count boundary (N+1..N+k): the frontend keeps
        // refreshing (time/frequency windows) while both imaging publishers
        // stay frozen at N and raw save continues.
        for(std::uint16_t trigger=105;trigger<=106;++trigger)
            send(trigger,1000000000LL+trigger);
        require(until([&]{return frontends[0]->snapshot().displayUpdates==6&&
                                  saver.savedCount()==6;}),
                "FO3 display keeps refreshing and save continues beyond the boundary");
        require(ringArrivals.load()==4,
                "FO3 imaging frozen at N: frontend Ring leg skipped");
        require(publisherFrames.load()==4,
                "FO3 imaging frozen at N: FramePublisher submit skipped");
        require(ringFinals.load()==0,
                "FO3 no synthetic final beyond the boundary");
        // FO4 Drop invariance (expired round): force the physical idle
        // timeout, then a late old-round sync reaches neither frontend nor
        // publisher.
        require(output.pollPhysicalRoundTimeout(4000000000LL),
                "FO4 physical idle timeout fired");
        output.sync(104,{make(9100,104,1000000000LL+104)},false);
        require(frontends[0]->snapshot().displayUpdates==6&&ringArrivals.load()==4&&
                publisherFrames.load()==4,
                "FO4 expired-round late sync reaches neither frontend nor publisher");
        // FO5 the gate reopens for the next physical round: the round reset
        // re-arms the startup filter (same admission semantics C8 relies on),
        // so the first new identity is a control Drop again and the second
        // one is the first logical Pass (logical index 0) of the new round.
        send(200,4000000000LL+1);
        send(201,4000000000LL+2);
        require(until([&]{return frontends[0]->snapshot().displayUpdates==7&&
                                  ringArrivals.load()==5&&publisherFrames.load()==5&&
                                  saver.savedCount()==7;}),
                "FO5 new round passes again after the timeout boundary");
        const auto stopped=output.stopSaving();require(until([&]{return output.savingApplied(stopped);}));
        output.stop();stopFrontends(frontends);
        publisher.requestInterruption();publisher.wait();
        // FO6 unchecked identity: with 「禁用计数重置」 unchecked, the same
        // input passes through bit-identically to the pre-change behavior --
        // every logical trigger reaches display, both imaging publishers and
        // save, and the Nth trigger still produces the one-shot CountBoundary.
        FileSaver saver2(0);
        DisplayBuffer display2;
        std::atomic<int> ringArrivals2{0},ringFinals2{0};
        std::vector<std::unique_ptr<FrontendPreprocessor>> frontends2;
        frontends2.push_back(makeFrontend(0,&display2,
            [&](const TriggerGroupConstPtr& frame){
                if(frame&&frame->roundComplete)++ringFinals2;
                ++ringArrivals2;
                return ImagingSubmitResult::Accepted;
            }));
        FramePublisher publisher2;
        publisher2.configure(true,1,16);
        std::atomic<int> publisherFrames2{0};
        QObject::connect(&publisher2,&FramePublisher::framePublished,
            qApp,[&](int,int){++publisherFrames2;},Qt::DirectConnection);
        publisher2.start();
        DataProcessor processor2(0,nullptr,&publisher2,config,
            [stage=frontends2.back().get()](const TriggerGroupPtr& frame){return stage->submit(frame);});
        HostOutput output2(32,50,{&processor2},{&saver2},nullptr,nullptr,4,
            {},0.0,1,false);
        wireFrontends(output2,frontends2);
        output2.beginSession(9200);output2.start();
        const auto save2=output2.startSaving(root.path(),100,"identity");
        require(until([&]{return output2.savingApplied(save2);}),
                "FO6 save configuration");
        auto send2=[&](std::uint16_t trigger,std::int64_t time){
            auto frame=make(9200,trigger,time);output2.card(frame);output2.sync(trigger,{frame},false);
        };
        send2(100,1000000000LL);
        for(std::uint16_t trigger=101;trigger<=106;++trigger)
            send2(trigger,1000000000LL+trigger);
        // Unchecked-mode identity: exactly five groups pass through (the Nth
        // trigger closes the round, and the first identity of the next
        // generation is re-admitted as a startup control by the UNCHANGED
        // normalizer admission semantics -- identical to the pre-change
        // behavior for this input), the Nth carries the one-shot final and
        // the generation advances.
        require(until([&]{return frontends2[0]->snapshot().displayUpdates==5&&
                                  ringArrivals2.load()==5&&publisherFrames2.load()==5&&
                                  saver2.savedCount()==5;}),
                "FO6 unchecked mode: identical pass-through for the same input");
        require(ringFinals2.load()==1,
                "FO6 unchecked mode: exactly one CountBoundary final at N");
        require(output2.normalizerSnapshot().roundGeneration==1,
                "FO6 unchecked mode: generation advanced at the count boundary");
        const auto stopped2=output2.stopSaving();require(until([&]{return output2.savingApplied(stopped2);}));
        output2.stop();stopFrontends(frontends2);
        publisher2.requestInterruption();publisher2.wait();
    }
    // ringLogicalTriggersPerRound is the single derivation of that threshold.
    {
        require(ringLogicalTriggersPerRound(8000, 2) == 4000,
                "G3 单圈总A-line数 8000 / 2 channels = 4000 designed triggers");
        require(ringLogicalTriggersPerRound(20000, 2) == 10000,
                "G3 derivation scales with 单圈总A-line数 instead of staying at 4000");
        require(ringLogicalTriggersPerRound(8000, 1) == 8000,
                "G3 a single channel equals the dialog value verbatim");
        require(ringLogicalTriggersPerRound(8001, 2) == 0,
                "G3 a non-divisible geometry is rejected");
        require(ringLogicalTriggersPerRound(0, 2) == 0 && ringLogicalTriggersPerRound(8000, 0) == 0,
                "G3 zero inputs are rejected");
    }
    // H4/H5 anti-revival: in the production save shape (direct sink installed,
    // legacy queue unused) the legacy branch of DataProcessor::deliverAssembled
    // must never run. That branch is what silently drops at FILESAVER_QUEUE_SIZE
    // and what stamps sessionGen from currentGeneration() instead of the
    // per-round resolveRound() binding.
    {
        AcqConfig config;
        config.acqTimeNs = 64;
        // Deliberately wire the legacy queue as well: if the direct sink did not
        // strictly win, these assertions would fail.
        moodycamel::ConcurrentQueue<TriggerGroupPtr> legacyQueue;
        std::vector<std::uint64_t> seen;
        DataProcessor processor(0, &legacyQueue, nullptr, config, {});
        processor.setDirectSaveSink([&seen](const TriggerGroupPtr& g) {
            seen.push_back(g->sessionGen);
            return true;
        });
        // The legacy reader's value must be ignored on the production path.
        processor.setSessionGenReader([] { return std::uint64_t(999); });
        processor.setSaveEnabled(true);

        auto stamped = [](std::uint64_t sessionGen) {
            auto g = std::make_shared<TriggerGroup>();
            g->cardId = 0;
            g->isComplete = true;
            g->sampleCount = 4;
            // HostOutput::card() writes this via tagSaveSession(resolveRound(...)).
            g->sessionGen = sessionGen;
            g->freqA.assign(4, 1.0f);
            g->freqB.assign(4, -1.0f);
            return g;
        };
        const auto r1 = processor.deliverAssembled(stamped(42), true, false);
        const auto r2 = processor.deliverAssembled(stamped(77), true, false);
        require(r1.save == DataProcessor::DeliveryResult::Consumed &&
                    r2.save == DataProcessor::DeliveryResult::Consumed,
                "S1 the direct save sink consumes even with a legacy queue wired");
        require(legacyQueue.size_approx() == 0,
                "S1 the legacy save queue is never used while the direct sink is set");
        require(seen.size() == 2 && seen[0] == 42 && seen[1] == 77,
                "S2 the round-resolved sessionGen survives; currentGeneration() is ignored");
    }
    // Full-resolution display preparation now lives in the Frontend Preprocessing
    // Stage and must preserve every acquired sample.
    {
        DisplayBuffer display;
        std::atomic<int> rings{0};
        auto frontend = makeFrontend(0, &display,
            [&](const TriggerGroupConstPtr&) { ++rings; return ImagingSubmitResult::Accepted; });
        auto group = std::make_shared<TriggerGroup>();
        group->sampleCount = 2048;
        group->measurementSession = 1;
        group->freqA.resize(2048);
        group->freqB.resize(2048);
        for (int i = 0; i < 2048; ++i) {
            group->freqA[i] = static_cast<float>(i);
            group->freqB[i] = static_cast<float>(-i);
        }
        frontend->beginSession(1);
        require(frontend->submit(group) == FrontendSubmitResult::Accepted,
                "frontend submit accepted");
        require(until([&] { return rings.load() == 1; }), "frontend display and Ring dispatch");
        DisplayBuffer::Snapshot snap;
        require(display.tryRead(snap));
        require(snap.sampleCount == 2048);
        require(snap.freqA.size() == 2048 && snap.freqB.size() == 2048);
        require(snap.phaseA.size() == 2048 && snap.phaseB.size() == 2048);
        require(snap.freqA.front() == 0.0 && snap.freqA.back() == 2047.0);
        frontend->stop();
    }
    std::cout<<"PASS production source/output/host boundary: four cards, 28/70, exact float16 files, display and Ring; normalized 1+N save/Ring identity; queued saving generation retains original directory; physical idle timeout shared by save/Ring; SourceCore assembly timeout isolated\n";
}
