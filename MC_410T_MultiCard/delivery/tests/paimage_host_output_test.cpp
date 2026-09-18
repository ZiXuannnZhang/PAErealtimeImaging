#include "PaimageAcquisition/HostOutput.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>
using namespace paimage;
using namespace std::chrono_literals;
void require(bool b,const char* message="host delivery contract"){
    if(!b)throw std::runtime_error(message);
}
template<class F> bool until(F f){auto end=std::chrono::steady_clock::now()+3s;while(!f()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);return f();}
int main(int argc,char** argv){QCoreApplication app(argc,argv);
    for(int samples:{5000,12500}){
        QTemporaryDir dir(QDir::currentPath()+"/host-output-XXXXXX");require(dir.isValid());
        std::vector<std::unique_ptr<DisplayBuffer>> displays;
        std::vector<std::unique_ptr<FileSaver>> savers;
        std::vector<std::unique_ptr<DataProcessor>> processors;
        std::vector<DataProcessor*> pp;std::vector<FileSaver*> ss;
        std::atomic<int> rings{0};std::atomic<bool> values{true};
        AcqConfig cfg;cfg.acqTimeNs=samples*4;
        for(int card=0;card<4;++card){
            displays.push_back(std::make_unique<DisplayBuffer>());savers.push_back(std::make_unique<FileSaver>(card));
            processors.push_back(std::make_unique<DataProcessor>(card,nullptr,displays.back().get(),nullptr,cfg,
                [&](const TriggerGroupConstPtr& frame){
                    if(!frame||frame->triggerSeq!=9||frame->freqA.size()!=std::size_t(samples)||frame->freqB.size()!=frame->freqA.size())values=false;
                    for(std::size_t i=0;i<frame->freqA.size();++i)if(frame->freqA[i]!=frame->cardId+1||frame->freqB[i]!=-frame->cardId-1)values=false;
                    ++rings;return ImagingSubmitResult::Accepted;
                }));pp.push_back(processors.back().get());ss.push_back(savers.back().get());
        }
        HostOutput output(32,50,pp,ss,nullptr);output.beginSession(1);output.start();
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
        require(until([&]{return rings==4 && savers[0]->savedCount()==1&&savers[1]->savedCount()==1&&savers[2]->savedCount()==1&&savers[3]->savedCount()==1;}));
        auto stopped=output.stopSaving();require(until([&]{return output.savingApplied(stopped);}));output.stop();require(values);
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
        DataProcessor processor(0,nullptr,&display,nullptr,config,{});
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
        DisplayBuffer display;FileSaver saver(0);AcqConfig config;config.acqTimeNs=64;config.displayPoints=16;
        std::mutex ringMutex;std::vector<std::uint16_t> ringTriggers;std::vector<std::int64_t> ringIndices;
        std::vector<bool> ringBoundaries;
        std::vector<PhysicalRoundEvent> roundEvents;
        DataProcessor processor(0,nullptr,&display,nullptr,config,
            [&](const TriggerGroupConstPtr& frame){
                std::lock_guard<std::mutex> lock(ringMutex);
                ringTriggers.push_back(frame->triggerSeq);
                ringIndices.push_back(frame->logicalTriggerIndex);
                ringBoundaries.push_back(frame->roundComplete);
                return ImagingSubmitResult::Accepted;
            });
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,3,
            [&](const PhysicalRoundEvent& event){roundEvents.push_back(event);});
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
        output.stop();
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
        DisplayBuffer display;FileSaver saver(0);AcqConfig config;config.acqTimeNs=64;config.displayPoints=16;
        std::mutex ringMutex;std::vector<std::uint16_t> ringTriggers;std::vector<std::int64_t> ringIndices;
        std::atomic<int> ringCount{0};
        std::vector<bool> ringBoundaries;std::vector<PhysicalRoundEvent> roundEvents;
        DataProcessor processor(0,nullptr,&display,nullptr,config,
            [&](const TriggerGroupConstPtr& frame){
                std::lock_guard<std::mutex> lock(ringMutex);
                ringTriggers.push_back(frame->triggerSeq);
                ringIndices.push_back(frame->logicalTriggerIndex);
                ringBoundaries.push_back(frame->roundComplete);
                ++ringCount;
                return ImagingSubmitResult::Accepted;
            });
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,3,
            [&](const PhysicalRoundEvent& event){roundEvents.push_back(event);},1.0e-6);
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
        deliver(200,20000);deliver(201,20500);deliver(202,20800);deliver(203,20900);
        require(until([&]{return saver.savedCount()==5&&ringCount.load()==5;}),
                "timeout T1/T7 save and Ring delivery");
        const auto stopped=output.stopSaving();require(until([&]{return output.savingApplied(stopped);}));
        output.stop();
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
        DataProcessor processor(0,nullptr,&display,nullptr,config,
            [&](const TriggerGroupConstPtr& frame){
                if (frame->sourceTimedOut) ++timedOutRings;
                ++ringCount;
                return ImagingSubmitResult::Accepted;
            });
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,4,
            [&](const PhysicalRoundEvent& event){roundEvents.push_back(event);},5.0);
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
        output.stop();
    }
    // C1-C3/C8/C13: real card FileSaver and sync/display/Ring paths; the
    // configured cap never limits raw bytes and is shared across both cards.
    for (int startup : {0,7}) for (bool disable : {false,true}) {
        QTemporaryDir root; require(root.isValid());
        AcqConfig config; config.acqTimeNs=64; config.displayPoints=16;
        DisplayBuffer d0,d1; FileSaver s0(0),s1(1);
        std::mutex mutex; std::vector<std::int64_t> indices;
        std::vector<std::uint64_t> generations; int finals=0,counts=0,timeouts=0;
        auto ring=[&](const TriggerGroupConstPtr& group) {
            std::lock_guard<std::mutex> lock(mutex);
            indices.push_back(group->logicalTriggerIndex); generations.push_back(group->roundGeneration);
            if(group->roundComplete)++finals;
            return ImagingSubmitResult::Accepted;
        };
        DataProcessor p0(0,nullptr,&d0,nullptr,config,ring),p1(1,nullptr,&d1,nullptr,config,ring);
        HostOutput output(32,50,{&p0,&p1},{&s0,&s1},nullptr,nullptr,4,
            [&](const auto& e){
                if(e.kind==PhysicalRoundEvent::Kind::CountBoundary)++counts;
                if(e.kind==PhysicalRoundEvent::Kind::TimeoutBoundary)++timeouts;
            },1.0,startup,disable);
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
            for(int i=0;i<8;++i)require(indices[i]==i/2 && generations[i]==0,"C3 multicard shared index");
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
        DataProcessor processor(0,nullptr,&display,nullptr,config,{});
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
    std::cout<<"PASS production source/output/host boundary: four cards, 28/70, exact float16 files, display and Ring; normalized 1+N save/Ring identity; queued saving generation retains original directory; physical idle timeout shared by save/Ring; SourceCore assembly timeout isolated\n";
}
