#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "PaimageAcquisition/OutputWorkers.h"
#include <chrono>
namespace paimage {
namespace {
void updateMax(std::atomic<std::uint64_t>& target,std::uint64_t value){auto current=target.load();while(value>current&&!target.compare_exchange_weak(current,value)){}}
}
OutputWorkers::OutputWorkers(int cards,int block,SourceCore::CardSink card,
    std::function<void(const SyncFrame&)> sync,Observer observer)
    :cardQueue_(cards,block),syncQueue_(cards,block),cardSink_(std::move(card)),
     syncSink_(std::move(sync)),observer_(std::move(observer)){}
OutputWorkers::~OutputWorkers(){stop();}
void OutputWorkers::event(Result r,Frame f){if(observer_)observer_(r,std::move(f));}
void OutputWorkers::start(){
    if(!stopping_)return;
    stopping_=false;
    try{cardWorker_=std::thread(&OutputWorkers::cardLoop,this);syncWorker_=std::thread(&OutputWorkers::syncLoop,this);}
    catch(...){stop();throw;}
}
void OutputWorkers::stop(){
    stopping_=true;cardReady_.notify_all();syncReady_.notify_all();
    if(cardWorker_.joinable())cardWorker_.join();
    if(syncWorker_.joinable())syncWorker_.join();
    {std::lock_guard<std::mutex> lock(cardMutex_);while(auto f=cardQueue_.popCard()){saveDepth_.fetch_sub(1);event(Result::ListenerDiscard,f);}}
    {std::lock_guard<std::mutex> lock(syncMutex_);while(auto f=syncQueue_.popSync())for(auto c:f->cards)event(Result::ListenerDiscard,c);}
}
void OutputWorkers::beginSession(std::uint64_t s){
    std::lock_guard<std::mutex> lock(syncMutex_);
    while(auto f=syncQueue_.popSync())for(auto c:f->cards)event(Result::SessionDiscard,c);
    syncQueue_.beginSession(s);session_=s;
    // 1430d7 clears sync queues only. Saving FIFO and in-flight card remain.
}
void OutputWorkers::setSavingEnabled(bool enabled){
    std::lock_guard<std::mutex> lock(cardMutex_);saving_=enabled;
    if(!enabled)while(auto f=cardQueue_.popCard()){saveDepth_.fetch_sub(1);event(Result::SavingDiscard,f);}
    // 143c7b..143d1d stops saving, clears FIFO and advances its independent
    // save generation. Host file lifecycle handshake is outside this boundary.
    cardReady_.notify_all();
}
std::uint64_t OutputWorkers::configureSaving(bool enabled,std::function<void()> apply){
    std::lock_guard<std::mutex> lock(cardMutex_);
    saving_=enabled;
    while(auto f=cardQueue_.popCard()){saveDepth_.fetch_sub(1);event(Result::SavingDiscard,f);}
    saveApply_=std::move(apply);
    const auto generation=++saveGeneration_;
    cardReady_.notify_all();
    return generation;
}
void OutputWorkers::pushCard(Frame f){
    std::lock_guard<std::mutex> lock(cardMutex_);
    if(!saving_){event(Result::CardDisabled,f);return;}
    if(stopping_){event(Result::ListenerDiscard,f);return;}
    if(!cardQueue_.pushCard(f)){saveQueueFull_.fetch_add(1);event(Result::CardFull,f);return;}
    saveEnqueued_.fetch_add(1);const auto depth=saveDepth_.fetch_add(1)+1;updateMax(savePeakDepth_,depth);
    event(Result::CardQueued,f);cardReady_.notify_one();
}
void OutputWorkers::pushSync(std::uint16_t trigger,const std::vector<Frame>& frames,bool startup){
    std::lock_guard<std::mutex> lock(syncMutex_);
    if(stopping_){for(auto f:frames)event(Result::ListenerDiscard,f);return;}
    auto evicted=syncQueue_.pushSync(trigger,frames,startup);
    for(auto& x:evicted)for(auto f:x.cards)event(Result::SyncEvicted,f);
    for(auto f:frames)event(Result::SyncQueued,f);
    syncReady_.notify_one();
}
void OutputWorkers::cardLoop(){
    // 14409d / 1440c3: 50ms condition wait. Source has no priority request here.
    while(true){Frame f;std::function<void()> apply;std::uint64_t generation=0;
        {std::unique_lock<std::mutex> lock(cardMutex_);
            cardReady_.wait_for(lock,std::chrono::milliseconds(50),[&]{return stopping_||saveGeneration_!=saveApplied_.load()||(saving_&&cardQueue_.cardDepth());});
            if(saveGeneration_!=saveApplied_.load()){generation=saveGeneration_;apply=saveApply_;}
            else if(saving_)f=cardQueue_.popCard();
            if(stopping_&&!generation&&!f)break;}
        if(generation){
            try{if(apply)apply();}
            catch(...){event(Result::CallbackFailed,{});setSavingEnabled(false);}
            saveApplied_=generation;
            continue;
        }
        if(!f){try{if(saveIdle_)saveIdle_();}catch(...){event(Result::CallbackFailed,{});setSavingEnabled(false);}continue;}
        saveDequeued_.fetch_add(1);saveDepth_.fetch_sub(1);const auto begin=std::chrono::steady_clock::now();
        try{if(cardSink_)cardSink_(f);event(Result::CardConsumed,f);}
        catch(...){event(Result::CallbackFailed,f);setSavingEnabled(false);}
        updateMax(maxSaveWorkerNs_,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());
    }
    try{if(saveExit_)saveExit_();}catch(...){event(Result::CallbackFailed,{});}
}
void OutputWorkers::syncLoop(){
    // 139c90: priority +1. No affinity request.
    syncPriorityError_=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL)?0:int(GetLastError());
    while(!stopping_){std::optional<SyncFrame> f;
        {std::unique_lock<std::mutex> lock(syncMutex_);
            syncReady_.wait_for(lock,std::chrono::milliseconds(50),[&]{return stopping_||syncQueue_.syncDepth();});
            if(stopping_)break;
            f=syncQueue_.popSync();}
        if(!f)continue;
        if(f->session!=session_.load()){for(auto c:f->cards)event(Result::SyncStale,c);continue;}
        try{if(syncSink_)syncSink_(*f);for(auto c:f->cards)event(Result::SyncConsumed,c);}
        catch(...){for(auto c:f->cards)event(Result::CallbackFailed,c);}
    }
}
OutputWorkers::Snapshot OutputWorkers::snapshot() const noexcept{Snapshot s;s.saveEnqueued=saveEnqueued_.load();s.saveDequeued=saveDequeued_.load();s.saveQueueFull=saveQueueFull_.load();s.saveCurrentDepth=saveDepth_.load();s.savePeakDepth=savePeakDepth_.load();s.maxSaveWorkerNs=maxSaveWorkerNs_.load();return s;}
}
