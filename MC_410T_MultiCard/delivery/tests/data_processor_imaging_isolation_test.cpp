#include "DataProcessor.h"
#include "ImagingBypass.h"
#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class P> bool until(P p){auto end=std::chrono::steady_clock::now()+3s;while(!p()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);return p();}
int main(int argc,char** argv){QCoreApplication app(argc,argv);
 ImagingBypass bypass(8);std::array<bool,8> channels{};channels[0]=channels[1]=true;bypass.setEnabledChannels(channels);bypass.setEnabled(true);bypass.setServiceReady(true);bypass.start();
 std::mutex mutex;std::condition_variable cv;bool release=false;std::atomic<int> entered{0},saved{0};
 bypass.setConsumer([&](const TriggerGroupConstPtr&,const std::array<bool,8>&){++entered;std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return release;});return true;});
 AcqConfig config;config.displayPoints=1;DataProcessor processor(0,nullptr,nullptr,nullptr,config,[&](const TriggerGroupConstPtr& f){return bypass.tryPush(f);});
 processor.setDirectSaveSink([&](const TriggerGroupPtr&){++saved;return true;});
 int imagingAccepted=0,queueRejected=0;for(int i=0;i<4000;++i){auto f=std::make_shared<TriggerGroup>();f->cardId=0;f->triggerSeq=std::uint16_t(i);f->measurementSession=1;f->isComplete=true;f->sampleCount=32;f->freqA.assign(32,1);f->freqB.assign(32,-1);auto r=processor.deliverAssembled(f,true,true);require(r.saveAccepted&&r.save==DataProcessor::DeliveryResult::Consumed,"save acceptance");imagingAccepted+=r.imagingAccepted;queueRejected+=r.imagingDropReason==ImagingSubmitResult::QueueFull||r.imagingDropReason==ImagingSubmitResult::QueueBusy;}
 DataProcessor throwing(0,nullptr,nullptr,nullptr,config,[](const TriggerGroupConstPtr&)->ImagingSubmitResult{throw std::runtime_error("imaging");});
 throwing.setDirectSaveSink([&](const TriggerGroupPtr&){++saved;return true;});auto e=std::make_shared<TriggerGroup>();e->cardId=0;e->isComplete=true;e->sampleCount=1;e->freqA={1};e->freqB={-1};auto er=throwing.deliverAssembled(e,true,true);
 require(er.saveAccepted&&er.imagingDropReason==ImagingSubmitResult::CallbackFailed,"imaging exception isolated");
 {std::lock_guard<std::mutex> lock(mutex);release=true;}cv.notify_all();require(until([&]{return entered.load()>=1;}),"worker delay");bypass.stop();
 require(saved==4001&&imagingAccepted<=9&&queueRejected>=3991,"4000-frame save/imaging isolation");
 std::cout<<"PASS 4000-frame pressure: saveAccepted=4000 while imaging queue saturated and worker delayed\n";
}
