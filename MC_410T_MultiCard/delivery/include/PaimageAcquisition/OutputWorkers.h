#pragma once
#include "OutputQueues.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace paimage {
// Source queue/worker boundary, before PAimage's downstream scaling/file format.
// Caller stops the receiver before destroying this object. Callbacks must not
// call control methods synchronously. Host format adapters run in these workers.
class OutputWorkers {
public:
    enum class Result { CardQueued, CardDisabled, CardFull, CardConsumed,
        SyncQueued, SyncEvicted, SyncConsumed, SyncStale, CallbackFailed,
        ListenerDiscard, SessionDiscard, SavingDiscard };
    using Observer=std::function<void(Result,Frame)>;
    OutputWorkers(int cards,int blockSize,SourceCore::CardSink,
                  std::function<void(const SyncFrame&)>,Observer={});
    ~OutputWorkers();
    void start();
    void stop();
    void requestStop(){stopping_=true;cardReady_.notify_all();syncReady_.notify_all();}
    void beginSession(std::uint64_t);
    void setSavingEnabled(bool);
    std::uint64_t configureSaving(bool,std::function<void()>);
    bool savingConfigurationApplied(std::uint64_t g) const {return saveApplied_.load()>=g;}
    // Install before start; all hooks execute on the sole saving worker.
    void setSavingHooks(std::function<void()> idle,std::function<void()> exit) {
        saveIdle_=std::move(idle);saveExit_=std::move(exit);
    }
    void pushCard(Frame);
    unsigned cardDepth(int card){std::lock_guard<std::mutex> lock(cardMutex_);return cardQueue_.cardDepth(card);}
    void pushSync(std::uint16_t,const std::vector<Frame>&,bool startupRelease);
    int syncPriorityError() const {return syncPriorityError_.load();}
    // Source checks again after downstream computation at 13aece. The host
    // adapter must use this immediately before publishing converted sync data.
    bool isCurrentSession(std::uint64_t s) const {return s==session_.load();}
private:
    void cardLoop();void syncLoop();void event(Result,Frame);
    OutputQueues cardQueue_,syncQueue_;
    SourceCore::CardSink cardSink_;std::function<void(const SyncFrame&)> syncSink_;Observer observer_;
    std::mutex cardMutex_,syncMutex_;std::condition_variable cardReady_,syncReady_;
    std::atomic<bool> stopping_{true};std::atomic<std::uint64_t> session_{0};
    std::atomic<int> syncPriorityError_{-1};
    bool saving_=false;
    std::uint64_t saveGeneration_=0;
    std::atomic<std::uint64_t> saveApplied_{0};
    std::function<void()> saveApply_,saveIdle_,saveExit_;
    std::thread cardWorker_,syncWorker_;
};
}
