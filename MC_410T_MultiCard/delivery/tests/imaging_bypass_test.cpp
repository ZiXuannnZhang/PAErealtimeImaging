#include "ImagingBypass.h"
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
TriggerGroupPtr makeFrame(int card,int trigger,std::uint64_t session,bool complete=true){auto f=std::make_shared<TriggerGroup>();f->cardId=card;f->triggerSeq=std::uint16_t(trigger);f->measurementSession=session;f->isComplete=complete;f->sampleCount=16;f->freqA.assign(16,float(card+1));f->freqB.assign(16,float(-card-1));return f;}
int main(){
 ImagingBypass bypass(2);std::array<bool,8> channels{};channels[0]=true;bypass.setEnabledChannels(channels);bypass.start();
 require(bypass.tryPush(makeFrame(0,1,1))==ImagingSubmitResult::Disabled,"disabled");bypass.setEnabled(true);
 require(bypass.tryPush(makeFrame(0,1,1))==ImagingSubmitResult::ServiceNotReady,"service not ready");bypass.setServiceReady(true);
 require(bypass.tryPush({})==ImagingSubmitResult::InvalidFrame,"null");require(bypass.tryPush(makeFrame(0,1,1,false))==ImagingSubmitResult::InvalidFrame,"incomplete");
 require(bypass.tryPush(makeFrame(1,1,1))==ImagingSubmitResult::Disabled,"disabled channels");
 std::mutex mutex;std::condition_variable cv;bool release=false;std::atomic<int> entered{0},processed{0},saves{0};
 bypass.setConsumer([&](const TriggerGroupConstPtr& f,const std::array<bool,8>& enabled){require(f&&enabled[f->cardId*2],"shared frame");++entered;std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return release;});++processed;return true;});
 ++saves;require(bypass.tryPush(makeFrame(0,10,10))==ImagingSubmitResult::Accepted,"accepted 1");require(until([&]{return entered.load()==1;}),"worker entered");
 ++saves;require(bypass.tryPush(makeFrame(0,11,10))==ImagingSubmitResult::Accepted,"accepted 2");++saves;require(bypass.tryPush(makeFrame(0,12,10))==ImagingSubmitResult::Accepted,"accepted 3");
 ++saves;require(bypass.tryPush(makeFrame(0,13,10))==ImagingSubmitResult::QueueFull,"queue full");require(saves==4,"save independent");
 bypass.beginSession(11);require(bypass.tryPush(makeFrame(0,14,10))==ImagingSubmitResult::StaleSession,"stale");require(bypass.tryPush(makeFrame(0,15,11))==ImagingSubmitResult::Accepted,"new session");
 bypass.setServiceReady(false);require(bypass.tryPush(makeFrame(0,16,11))==ImagingSubmitResult::ServiceNotReady,"service stopped");
 {std::lock_guard<std::mutex> lock(mutex);release=true;}cv.notify_all();require(until([&]{return processed.load()==1;}),"in flight");bypass.stop();
 require(bypass.tryPush(makeFrame(0,17,11))==ImagingSubmitResult::Stopping,"stopping");const auto s=bypass.snapshot();
 require(s.accepted==4,"accepted count");require(s.droppedQueueFull>=1&&s.droppedServiceNotReady>=2,"drop reasons");require(s.droppedStaleSession>=1&&s.droppedInvalidFrame>=2,"filters");
 require(s.peakDepth==2&&s.currentDepth==0,"depth");require(s.maxSubmitNs>0&&s.maxWorkerNs>0,"timing");
 std::cout<<"PASS imaging bypass: bounded admission, explicit drops, session/service isolation, early filters, saving independence\n";
}
