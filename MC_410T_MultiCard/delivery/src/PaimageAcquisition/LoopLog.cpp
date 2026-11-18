#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "PaimageAcquisition/LoopLog.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace paimage {
namespace {
std::mutex registryMutex;
std::vector<LoopLog*> registry;

constexpr std::uint64_t kDropIncrement = std::uint64_t{1} << 32;
constexpr std::uint64_t accepted(std::uint64_t state) { return std::uint32_t(state); }
constexpr std::uint64_t droppedCount(std::uint64_t state) { return state >> 32; }
constexpr std::uint64_t issued(std::uint64_t state) { return accepted(state) + droppedCount(state); }
constexpr bool exhausted(std::uint64_t state) {
    // Keep headroom for concurrent producers already committing a drop.
    return accepted(state) == (std::numeric_limits<std::uint32_t>::max)() ||
           droppedCount(state) >= (std::uint64_t{1} << 31);
}
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "LoopLog admission must not block");

std::int64_t qpc() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
std::int64_t qpf() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}
std::int64_t utc100ns() {
    FILETIME fileTime{};
    GetSystemTimePreciseAsFileTime(&fileTime);
    ULARGE_INTEGER value{};
    value.LowPart=fileTime.dwLowDateTime;
    value.HighPart=fileTime.dwHighDateTime;
    return std::int64_t(value.QuadPart);
}
std::uint64_t steadyNs() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::uint64_t burstEpoch(const LoopRecord& record) {
    return (std::uint64_t(record.value1)<<32) | std::uint64_t(record.value0);
}
}

LoopLog::LoopLog(std::filesystem::path path):LoopLog(std::move(path),Budget{}){}

LoopLog::LoopLog(std::filesystem::path path,Budget budget)
    :root_(std::move(path)),budget_(budget),qpcStart_(qpc()),qpcFrequency_(qpf()),utcStart100ns_(utc100ns()) {
    if (budget_.records<2 || (budget_.records&(budget_.records-1))
        || budget_.segmentBytes<kRecordBytes || budget_.totalBytes<kRecordBytes)
        throw std::invalid_argument("loop log budget");
    slots_=std::make_unique<Slot[]>(budget_.records);
    for (std::size_t index=0;index<budget_.records;++index) slots_[index].sequence.store(index);
    worker_=std::thread(&LoopLog::run,this);
    std::lock_guard<std::mutex> lock(registryMutex);
    registry.push_back(this);
}

LoopLog::~LoopLog() {
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.erase(std::remove(registry.begin(),registry.end(),this),registry.end());
    }
    stop();
}

std::vector<LoopLog::Cut> LoopLog::captureCuts() {
    std::lock_guard<std::mutex> lock(registryMutex);
    std::vector<Cut> cuts;
    cuts.reserve(registry.size());
    for (LoopLog* log:registry) {
        const auto progress=log->exportProgress_;
        const auto admission=log->admission_.load(std::memory_order_acquire);
        const auto boundary=issued(admission);
        const auto position=std::size_t(accepted(admission));
        auto requested=progress->requested.load(std::memory_order_relaxed);
        while (requested<position
               && !progress->requested.compare_exchange_weak(requested,position,
                                                               std::memory_order_release,
                                                               std::memory_order_relaxed)) {}
        cuts.push_back({log->root_,boundary,droppedCount(admission),exhausted(admission),
            [progress,position]{
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while (!progress->done.load(std::memory_order_acquire)
                       && progress->flushed.load(std::memory_order_acquire)<position
                       && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return progress->flushed.load(std::memory_order_acquire)>=position;
            },
            [progress,boundary]{
                std::vector<WindowMeta> windows;
                bool windowOpen;
                {
                    std::lock_guard<std::mutex> lock(progress->inventoryMutex);
                    // A later freeze can satisfy this cut if the window
                    // already had a burst marker at the cut's sequence.
                    for (const auto& window : progress->windows)
                        if (window.triggerSequence<=boundary) windows.push_back(window);
                    windowOpen=progress->openWindowTriggerSequence!=0 &&
                               progress->openWindowTriggerSequence<=boundary;
                }
                std::uint64_t recordsWritten=0,budgetOmitted=0,pending=0;
                for (auto& window : windows) {
                    while (window.markers.back().sequence>boundary) window.markers.pop_back();
                    window.endNs=window.markers.back().endNs;
                    if (window.firstOmittedSequence>boundary) window.firstOmittedSequence=0;
                    if (window.budgetOmitted) budgetOmitted+=window.recordCount;
                    else if (window.file.empty()) pending+=window.recordCount;
                    else recordsWritten+=window.recordCount;
                }
                std::ostringstream inventory;
                inventory<<"{\"recordsWritten\":"<<recordsWritten
                         <<",\"budgetOmitted\":"<<budgetOmitted<<",\"pending\":"<<pending
                         <<",\"budgetExhausted\":"<<(budgetOmitted?"true":"false")
                         <<",\"writeFailed\":"<<(pending?"true":"false")
                         <<",\"openWindowPending\":"<<(windowOpen?"true":"false")<<",\"windows\":";
                writeWindowList(inventory,windows);
                inventory<<'}';
                return inventory.str();
            }});
    }
    return cuts;
}

void LoopLog::stop() {
    stopping_.store(true,std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

std::uint64_t LoopLog::dropped() const { return droppedCount(admission_.load(std::memory_order_relaxed)); }
std::uint64_t LoopLog::recordsIssued() const { return issued(admission_.load(std::memory_order_relaxed)); }
bool LoopLog::incomplete() const {
    const auto state=admission_.load(std::memory_order_relaxed);
    return droppedCount(state)!=0||exhausted(state)||writeFailed_.load()||budgetExhausted_.load();
}

std::uint64_t LoopLog::push(LoopRecord record) noexcept {
    if(paused_.load(std::memory_order_relaxed)) return 0;
    auto state=admission_.load(std::memory_order_relaxed);
    for (int attempt=0;attempt<8;++attempt) {
        if (exhausted(state)) return 0;
        const auto position=accepted(state);
        auto& slot=slots_[position&(budget_.records-1)];
        const auto slotSequence=slot.sequence.load(std::memory_order_acquire);
        const auto delta=std::intptr_t(slotSequence)-std::intptr_t(position);
        if (delta<0) break;
        if (delta==0) {
            if (admission_.compare_exchange_weak(state,state+1,std::memory_order_relaxed)) {
                record.sequence=issued(state)+1;
#ifdef PAIMAGE_SOCKET_TEST_SEAM
                if (budget_.beforePublish) budget_.beforePublish(record.sequence);
#endif
                slot.record=record;
                // Calculate backlog before releasing the slot to the writer.
                const auto used=position+1-consumed_.load(std::memory_order_relaxed);
                slot.sequence.store(position+1,std::memory_order_release);
                auto peak=peak_.load(std::memory_order_relaxed);
                if (used>peak) peak_.compare_exchange_strong(peak,used,std::memory_order_relaxed);
                if (record.kind==std::uint16_t(LoopKind::Loop))
                    lastProgress_.store(record.timeNs,std::memory_order_relaxed);
                return record.sequence;
            }
        } else {
            state=admission_.load(std::memory_order_relaxed);
        }
    }
    if (exhausted(admission_.load(std::memory_order_relaxed))) return 0;
    return issued(admission_.fetch_add(kDropIncrement,std::memory_order_relaxed))+1;
}

void LoopLog::noteThreadStart(std::uint64_t nowNs) noexcept {
    listenerStartNs_.store(nowNs,std::memory_order_release);
    lastSampleNs_.store(nowNs,std::memory_order_release);
}

void LoopLog::noteSampleData(std::uint64_t nowNs) noexcept {
    if(paused_.load(std::memory_order_relaxed)) return;
    const auto previous=lastSampleNs_.exchange(nowNs,std::memory_order_acq_rel);
    if (!previous || nowNs<previous || nowNs-previous<2ull*1000000000ull) return;
    const auto epoch=freezeEpoch_.fetch_add(1,std::memory_order_acq_rel)+1;
    LoopRecord marker;
    marker.timeNs=nowNs;
    marker.kind=std::uint16_t(LoopKind::BurstMark);
    marker.threadId=GetCurrentThreadId();
    marker.value0=std::uint32_t(epoch);
    marker.value1=std::uint32_t(epoch>>32);
    marker.spanNs=nowNs-previous;
    marker.payloadA=previous;
    marker.payloadB=nowNs-kPreWindowNs;
    push(marker);
}

void LoopLog::trimRolling(std::uint64_t referenceNs) {
    while (!rolling_.empty()) {
        const auto time=rolling_.front().timeNs;
        if (!time || referenceNs<=time || referenceNs-time<=kPreWindowNs) break;
        rolling_.pop_front();
        rollingBytes_-=kRecordBytes;
        ++retentionEvicted_;
    }
    const auto maxRecords=std::size_t(kWindowBytes/kRecordBytes);
    while (rolling_.size()>maxRecords) {
        rolling_.pop_front();
        rollingBytes_-=kRecordBytes;
        ++retentionEvicted_;
    }
}

std::size_t LoopLog::windowRecordLimit() const {
    constexpr auto maxRecords=std::size_t(kWindowBytes/kRecordBytes);
#ifdef PAIMAGE_SOCKET_TEST_SEAM
    if (budget_.windowRecordLimit) return std::min(maxRecords,budget_.windowRecordLimit);
#endif
    return maxRecords;
}

void LoopLog::openWindow(const LoopRecord& marker, std::size_t queuePosition) {
    ActiveWindow window;
    const auto burstTime=marker.timeNs;
    window.meta.id=nextWindowId_++;
    window.meta.triggerSequence=marker.sequence;
    window.meta.startNs=burstTime>kPreWindowNs?burstTime-kPreWindowNs:0;
    window.meta.endNs=burstTime+kPostWindowNs;
    window.meta.queueDroppedAtOpen=marker.sequence-(queuePosition+1);
    window.meta.retentionEvictedAtOpen=retentionEvicted_;
    const auto listenerStart=listenerStartNs_.load(std::memory_order_acquire);
    window.meta.beforeListenerStart=listenerStart!=0 && window.meta.startNs<listenerStart;
    window.meta.preWindowTruncated=window.meta.beforeListenerStart
        || rolling_.empty()
        || (rolling_.front().timeNs && rolling_.front().timeNs>window.meta.startNs);
    // The run's cumulative drops do not establish when a pre-window
    // record was lost. Report transport loss separately from coverage.
    window.meta.markers.push_back({marker.sequence,burstEpoch(marker),window.meta.endNs});
    const auto maxRecords=windowRecordLimit();
    for (const auto& record:rolling_) {
        if (record.timeNs<window.meta.startNs || record.timeNs>burstTime) continue;
        if (window.records.size()<maxRecords) window.records.push_back(record);
        else if (!window.meta.firstOmittedSequence) window.meta.firstOmittedSequence=record.sequence;
    }
    activeWindow_=std::move(window);
    {
        std::lock_guard<std::mutex> lock(exportProgress_->inventoryMutex);
        exportProgress_->openWindowTriggerSequence=marker.sequence;
    }
    appendToWindow(marker);
}

void LoopLog::appendToWindow(const LoopRecord& record) {
    if (!activeWindow_) return;
    auto& window=*activeWindow_;
    if (record.timeNs<window.meta.startNs || record.timeNs>window.meta.endNs) return;
    if (!window.records.empty() && window.records.back().sequence==record.sequence) return;
    const auto maxRecords=windowRecordLimit();
    if (window.records.size()>=maxRecords) {
        if (!window.meta.firstOmittedSequence) window.meta.firstOmittedSequence=record.sequence;
        return;
    }
    window.records.push_back(record);
}

bool LoopLog::writeWindow(const ActiveWindow& window) {
    if (window.records.empty()) return true;
    const auto name=std::string("looplog-window-")+std::to_string(window.meta.id)+".bin";
    const auto temporary=root_/(name+".part");
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(window.records.data()),
                 std::streamsize(window.records.size()*kRecordBytes));
    output.close();
    // The exporter must see either a frozen file or no file. A concurrent
    // export must never mistake bytes still being written for corruption.
    std::error_code error;
    if (output) {
        std::filesystem::rename(temporary,root_/name,error);
        if (!error) return true;
    }
    std::filesystem::remove(temporary,error);
    return false;
}

void LoopLog::finalizeWindow(bool force) {
    if (!activeWindow_) return;
    auto window=std::move(*activeWindow_);
    activeWindow_.reset();
    if (force) window.meta.stopCoverageNs=lastConsumedNs_;
    window.meta.recordCount=window.records.size();
    window.meta.bytes=window.records.size()*kRecordBytes;
    const auto frozenLimit=std::min<std::uint64_t>(kFrozenBytes,budget_.totalBytes);
    if (frozenBytes_+window.meta.bytes>frozenLimit) {
        window.meta.budgetOmitted=true;
        budgetExhausted_.store(true,std::memory_order_release);
        budgetOmittedRecords_+=window.meta.recordCount;
    } else if (!writeWindow(window)) {
        writeFailed_.store(true,std::memory_order_release);
        pendingRecords_+=window.meta.recordCount;
    } else {
        window.meta.file=std::string("looplog-window-")+std::to_string(window.meta.id)+".bin";
        frozenBytes_+=window.meta.bytes;
        windowWriteRecords_+=window.meta.recordCount;
    }
    {
        std::lock_guard<std::mutex> lock(exportProgress_->inventoryMutex);
        exportProgress_->windows.push_back(std::move(window.meta));
        exportProgress_->openWindowTriggerSequence=0;
    }
}

void LoopLog::consumeRecord(const LoopRecord& record, std::size_t queuePosition) {
    if (record.timeNs>lastConsumedNs_) lastConsumedNs_=record.timeNs;
    trimRolling(record.timeNs);
    rolling_.push_back(record);
    rollingBytes_+=kRecordBytes;
    trimRolling(record.timeNs);

    const auto isBurst=record.kind==std::uint16_t(LoopKind::BurstMark);
    if (isBurst) {
        if (activeWindow_ && record.timeNs>activeWindow_->meta.endNs) finalizeWindow(false);
        if (activeWindow_) {
            activeWindow_->meta.endNs=std::max(activeWindow_->meta.endNs,record.timeNs+kPostWindowNs);
            activeWindow_->meta.markers.push_back({record.sequence,burstEpoch(record),activeWindow_->meta.endNs});
            appendToWindow(record);
        } else {
            openWindow(record,queuePosition);
        }
    } else if (activeWindow_) {
        appendToWindow(record);
    }
}

void LoopLog::writeSummary(std::uint64_t recordsWritten,std::uint64_t writerUnwritten) {
    std::ofstream summary(root_/"looplog-summary.json",std::ios::trunc);
    if (!summary) {
        writeFailed_.store(true,std::memory_order_release);
        return;
    }
    const auto segmentRecords=budget_.segmentBytes/kRecordBytes;
    const auto totalRecords=budget_.totalBytes/kRecordBytes;
    const auto admission=admission_.load(std::memory_order_relaxed);
    summary<<"{\"schemaVersion\":2,\"mode\":\"rolling-burst-windows\","
        <<"\"recordBytes\":80,\"recordsIssued\":"<<issued(admission)
        <<",\"recordsWritten\":"<<recordsWritten
        <<",\"queueDropped\":"<<droppedCount(admission)
        <<",\"counterExhausted\":"<<(exhausted(admission)?"true":"false")
        <<",\"writerUnwritten\":"<<writerUnwritten
        <<",\"budgetOmitted\":"<<budgetOmittedRecords_
        <<",\"pending\":"<<pendingRecords_
        <<",\"retentionEvicted\":"<<retentionEvicted_
        <<",\"loopLogIncomplete\":"<<(incomplete()?"true":"false")
        <<",\"queuePeak\":"<<peak_.load()
        <<",\"queueAllocatedBytes\":"<<budget_.records*sizeof(Slot)
        <<",\"segmentBudgetBytes\":"<<budget_.segmentBytes
        <<",\"segmentRecordBudget\":"<<segmentRecords
        <<",\"segmentTailBytes\":"<<(budget_.segmentBytes-segmentRecords*kRecordBytes)
        <<",\"totalBudgetBytes\":"<<budget_.totalBytes
        <<",\"totalRecordBudget\":"<<totalRecords
        <<",\"totalTailBytes\":"<<(budget_.totalBytes-totalRecords*kRecordBytes)
        <<",\"budgetExhausted\":"<<(budgetExhausted_.load()?"true":"false")
        <<",\"writeFailed\":"<<(writeFailed_.load()?"true":"false")
        <<",\"burstMarkEpochs\":"<<freezeEpoch_.load()
        <<",\"continuousListenerLoopLog\":false"
        <<",\"preWindowSeconds\":5,\"postWindowSeconds\":5"
        <<",\"rollingWindowMaxBytes\":"<<kWindowBytes
        <<",\"frozenWindowBudgetBytes\":"<<kFrozenBytes
        <<",\"qpcFrequency\":"<<qpcFrequency_
        <<",\"anchors\":[{\"qpc\":"<<qpcStart_<<",\"utcFileTime100ns\":"<<utcStart100ns_
        <<"},{\"qpc\":"<<qpc()<<",\"utcFileTime100ns\":"<<utc100ns()<<"}]"
        <<",\"windows\":";
    writeWindowList(summary,exportProgress_->windows);
    summary<<"}\n";
    summary.flush();
    if (!summary) writeFailed_.store(true,std::memory_order_release);
}

void LoopLog::writeWindowList(std::ostream& summary,const std::vector<WindowMeta>& windows) {
    summary<<'[';
    for (std::size_t index=0;index<windows.size();++index) {
        if (index) summary<<',';
        const auto& window=windows[index];
        summary<<"{\"windowId\":"<<window.id<<",\"triggerSequence\":"<<window.triggerSequence
            <<",\"startNs\":"<<window.startNs
            <<",\"endNs\":"<<window.endNs<<",\"recordCount\":"<<window.recordCount
            <<",\"bytes\":"<<window.bytes<<",\"file\":\""<<window.file
            <<"\",\"preWindowTruncated\":"<<(window.preWindowTruncated?"true":"false")
            <<",\"beforeListenerStart\":"<<(window.beforeListenerStart?"true":"false")
            <<",\"postWindowTruncated\":"
            <<((window.stopCoverageNs && *window.stopCoverageNs<window.endNs)?"true":"false")
            <<",\"windowTruncated\":"<<(window.firstOmittedSequence?"true":"false")
            <<",\"budgetOmitted\":"<<(window.budgetOmitted?"true":"false")
            <<",\"queueDroppedAtOpen\":"<<window.queueDroppedAtOpen
            <<",\"retentionEvictedAtOpen\":"<<window.retentionEvictedAtOpen
            <<",\"burstEpochs\":[";
        for (std::size_t epoch=0;epoch<window.markers.size();++epoch) {
            if (epoch) summary<<',';
            summary<<window.markers[epoch].epoch;
        }
        summary<<"]}";
    }
    summary<<']';
}

void LoopLog::run() {
    std::error_code error;
    std::filesystem::create_directories(root_,error);
    bool failed=bool(error);
    std::uint64_t writerUnwritten=0;
    std::vector<LoopRecord> batch;
    batch.reserve(4096);
    std::uint64_t fencedWindowEnd=0;
    std::size_t windowEndPosition=0;
    while (true) {
#ifdef PAIMAGE_SOCKET_TEST_SEAM
        if (budget_.beforeDrain) budget_.beforeDrain();
#endif
        batch.clear();
        while (batch.size()<4096) {
            auto& slot=slots_[read_&(budget_.records-1)];
            if (slot.sequence.load(std::memory_order_acquire)!=read_+1) break;
            batch.push_back(slot.record);
            slot.sequence.store(read_+budget_.records,std::memory_order_release);
            ++read_;
        }
        consumed_.store(read_,std::memory_order_release);
        for (std::size_t i=0;i<batch.size();++i)
            consumeRecord(batch[i],read_-batch.size()+i);
        const bool stopping=stopping_.load(std::memory_order_acquire);
        if (activeWindow_ && steadyNs()>=activeWindow_->meta.endNs) {
            // At the deadline the queue may still contain records for this
            // window. Drain to the producer position observed at expiry,
            // independently of export clicks and subsequent ordinary traffic.
            if (fencedWindowEnd!=activeWindow_->meta.endNs) {
                fencedWindowEnd=activeWindow_->meta.endNs;
                windowEndPosition=accepted(admission_.load(std::memory_order_acquire));
            }
            if (read_>=windowEndPosition) finalizeWindow(stopping);
        }

        const auto requested=exportProgress_->requested.load(std::memory_order_acquire);
        if (requested>exportProgress_->flushed.load(std::memory_order_relaxed)
            && read_>=requested) {
            exportProgress_->flushed.store(read_,std::memory_order_release);
        }
        if (stopping && !activeWindow_ && read_==accepted(admission_.load(std::memory_order_acquire))) break;
        if (batch.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (activeWindow_) finalizeWindow(true);
    if (failed) writeFailed_.store(true,std::memory_order_release);
    writeSummary(windowWriteRecords_,writerUnwritten);
    exportProgress_->flushed.store(read_,std::memory_order_release);
    exportProgress_->done.store(true,std::memory_order_release);
}

}
