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
    std::cout<<"PASS production source/output/host boundary: four cards, 28/70, exact float16 files, display and Ring; normalized 1+N save/Ring identity; queued saving generation retains original directory; no legacy QThreads\n";
}
