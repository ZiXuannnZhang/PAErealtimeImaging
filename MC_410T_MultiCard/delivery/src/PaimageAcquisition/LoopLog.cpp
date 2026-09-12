#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "PaimageAcquisition/LoopLog.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace paimage {
namespace {
std::mutex registryMutex;
std::vector<LoopLog*> registry;

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
        const auto boundary=log->sequence_.load(std::memory_order_acquire);
        const auto position=log->write_.load(std::memory_order_acquire);
        auto requested=progress->requested.load(std::memory_order_relaxed);
        while (requested<position
               && !progress->requested.compare_exchange_weak(requested,position,
                                                               std::memory_order_release,
                                                               std::memory_order_relaxed)) {}
        cuts.push_back({log->root_,boundary,log->dropped_.load(),log->freezeEpoch_.load(),
            log->incomplete(),log->budgetExhausted_.load(),log->writeFailed_.load(),
            [progress,position]{
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while (!progress->done.load(std::memory_order_acquire)
                       && progress->flushed.load(std::memory_order_acquire)<position
                       && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return (progress->done.load(std::memory_order_acquire)
                        || progress->flushed.load(std::memory_order_acquire)>=position)
                    && !progress->failed.load(std::memory_order_acquire);
            }});
    }
    return cuts;
}

void LoopLog::stop() {
    stopping_.store(true,std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

std::uint64_t LoopLog::push(LoopRecord record) noexcept {
    record.sequence=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
    auto position=write_.load(std::memory_order_relaxed);
    for (int attempt=0;attempt<8;++attempt) {
        auto& slot=slots_[position&(budget_.records-1)];
        const auto slotSequence=slot.sequence.load(std::memory_order_acquire);
        const auto delta=std::intptr_t(slotSequence)-std::intptr_t(position);
        if (delta<0) break;
        if (delta==0 && write_.compare_exchange_weak(position,position+1,
                                                      std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
            slot.record=record;
            slot.sequence.store(position+1,std::memory_order_release);
            const auto used=position+1-consumed_.load(std::memory_order_relaxed);
            auto peak=peak_.load(std::memory_order_relaxed);
            if (used>peak) peak_.compare_exchange_strong(peak,used,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed);
            if (record.kind==std::uint16_t(LoopKind::Loop))
                lastProgress_.store(record.timeNs,std::memory_order_relaxed);
            return record.sequence;
        }
        position=write_.load(std::memory_order_relaxed);
    }
    dropped_.fetch_add(1,std::memory_order_relaxed);
    return record.sequence;
}

void LoopLog::noteThreadStart(std::uint64_t nowNs) noexcept {
    listenerStartNs_.store(nowNs,std::memory_order_release);
    lastSampleNs_.store(nowNs,std::memory_order_release);
}

void LoopLog::noteSampleData(std::uint64_t nowNs) noexcept {
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

void LoopLog::openWindow(const LoopRecord& marker) {
    ActiveWindow window;
    const auto burstTime=marker.timeNs;
    window.meta.id=nextWindowId_++;
    window.meta.startNs=burstTime>kPreWindowNs?burstTime-kPreWindowNs:0;
    window.meta.endNs=burstTime+kPostWindowNs;
    window.meta.queueDroppedAtOpen=dropped_.load(std::memory_order_relaxed);
    window.meta.retentionEvictedAtOpen=retentionEvicted_;
    const auto listenerStart=listenerStartNs_.load(std::memory_order_acquire);
    window.meta.beforeListenerStart=listenerStart!=0 && window.meta.startNs<listenerStart;
    window.meta.preWindowTruncated=window.meta.beforeListenerStart
        || rolling_.empty()
        || (rolling_.front().timeNs && rolling_.front().timeNs>window.meta.startNs)
        || window.meta.queueDroppedAtOpen!=0;
    window.meta.burstEpochs.push_back(burstEpoch(marker));
    const auto maxRecords=std::size_t(kWindowBytes/kRecordBytes);
    for (const auto& record:rolling_) {
        if (record.timeNs<window.meta.startNs || record.timeNs>burstTime) continue;
        if (window.records.size()<maxRecords) window.records.push_back(record);
        else window.meta.windowTruncated=true;
    }
    activeWindow_=std::move(window);
    appendToWindow(marker);
}

void LoopLog::appendToWindow(const LoopRecord& record) {
    if (!activeWindow_) return;
    auto& window=*activeWindow_;
    if (record.timeNs<window.meta.startNs || record.timeNs>window.meta.endNs) return;
    const auto maxRecords=std::size_t(kWindowBytes/kRecordBytes);
    if (window.records.size()>=maxRecords) {
        window.meta.windowTruncated=true;
        return;
    }
    if (!window.records.empty() && window.records.back().sequence==record.sequence) return;
    window.records.push_back(record);
}

bool LoopLog::writeWindow(const ActiveWindow& window) {
    if (window.records.empty()) return true;
    const auto name=std::string("looplog-window-")+std::to_string(window.meta.id)+".bin";
    std::ofstream output(root_/name,std::ios::binary|std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(window.records.data()),
                 std::streamsize(window.records.size()*kRecordBytes));
    output.flush();
    if (!output) return false;
    return true;
}

void LoopLog::finalizeWindow(bool force) {
    if (!activeWindow_) return;
    auto window=std::move(*activeWindow_);
    activeWindow_.reset();
    if (force && lastConsumedNs_<window.meta.endNs) window.meta.postWindowTruncated=true;
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
    completedWindows_.push_back(std::move(window.meta));
}

void LoopLog::consumeRecord(const LoopRecord& record) {
    if (record.timeNs>lastConsumedNs_) lastConsumedNs_=record.timeNs;
    trimRolling(record.timeNs);
    rolling_.push_back(record);
    rollingBytes_+=kRecordBytes;
    trimRolling(record.timeNs);

    const auto isBurst=record.kind==std::uint16_t(LoopKind::BurstMark);
    if (activeWindow_ && record.timeNs>activeWindow_->meta.endNs) finalizeWindow(false);
    if (isBurst) {
        if (activeWindow_) {
            activeWindow_->meta.endNs=std::max(activeWindow_->meta.endNs,record.timeNs+kPostWindowNs);
            activeWindow_->meta.burstEpochs.push_back(burstEpoch(record));
            appendToWindow(record);
        } else {
            openWindow(record);
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
    summary<<"{\"schemaVersion\":2,\"mode\":\"rolling-burst-windows\","
        <<"\"recordBytes\":80,\"recordsIssued\":"<<sequence_.load()
        <<",\"recordsWritten\":"<<recordsWritten
        <<",\"queueDropped\":"<<dropped_.load()
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
        <<",\"windows\":[";
    for (std::size_t index=0;index<completedWindows_.size();++index) {
        if (index) summary<<',';
        const auto& window=completedWindows_[index];
        summary<<"{\"windowId\":"<<window.id<<",\"startNs\":"<<window.startNs
            <<",\"endNs\":"<<window.endNs<<",\"recordCount\":"<<window.recordCount
            <<",\"bytes\":"<<window.bytes<<",\"file\":\""<<window.file
            <<"\",\"preWindowTruncated\":"<<(window.preWindowTruncated?"true":"false")
            <<",\"beforeListenerStart\":"<<(window.beforeListenerStart?"true":"false")
            <<",\"postWindowTruncated\":"<<(window.postWindowTruncated?"true":"false")
            <<",\"windowTruncated\":"<<(window.windowTruncated?"true":"false")
            <<",\"budgetOmitted\":"<<(window.budgetOmitted?"true":"false")
            <<",\"queueDroppedAtOpen\":"<<window.queueDroppedAtOpen
            <<",\"retentionEvictedAtOpen\":"<<window.retentionEvictedAtOpen
            <<",\"burstEpochs\":[";
        for (std::size_t epoch=0;epoch<window.burstEpochs.size();++epoch) {
            if (epoch) summary<<',';
            summary<<window.burstEpochs[epoch];
        }
        summary<<"]}";
    }
    summary<<"]}\n";
    summary.flush();
    if (!summary) writeFailed_.store(true,std::memory_order_release);
}

void LoopLog::run() {
    std::error_code error;
    std::filesystem::create_directories(root_,error);
    bool failed=bool(error);
    std::uint64_t writerUnwritten=0;
    std::vector<LoopRecord> batch;
    batch.reserve(4096);
    while (true) {
        const auto requested=exportProgress_->requested.load(std::memory_order_acquire);
        if (requested>exportProgress_->flushed.load(std::memory_order_relaxed)
            && read_>=requested) {
            exportProgress_->flushed.store(read_,std::memory_order_release);
            exportProgress_->failed.store(failed||writeFailed_.load(),std::memory_order_release);
        }
        batch.clear();
        while (batch.size()<4096) {
            auto& slot=slots_[read_&(budget_.records-1)];
            if (slot.sequence.load(std::memory_order_acquire)!=read_+1) break;
            batch.push_back(slot.record);
            slot.sequence.store(read_+budget_.records,std::memory_order_release);
            ++read_;
        }
        consumed_.store(read_,std::memory_order_release);
        for (const auto& record:batch) consumeRecord(record);
        if (activeWindow_ && lastConsumedNs_>=activeWindow_->meta.endNs) finalizeWindow(false);

        if (batch.empty()) {
            if (activeWindow_ && steadyNs()>=activeWindow_->meta.endNs) finalizeWindow(false);
            if (stopping_.load(std::memory_order_acquire) && read_==write_.load(std::memory_order_acquire)) {
                if (activeWindow_) {
                    if (steadyNs()<activeWindow_->meta.endNs) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    finalizeWindow(true);
                }
                pendingRecords_=write_.load(std::memory_order_acquire)-read_;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
    }
    if (activeWindow_) finalizeWindow(true);
    if (failed) writeFailed_.store(true,std::memory_order_release);
    writeSummary(windowWriteRecords_,writerUnwritten);
    exportProgress_->failed.store(incomplete(),std::memory_order_release);
    exportProgress_->flushed.store(read_,std::memory_order_release);
    exportProgress_->done.store(true,std::memory_order_release);
}

}
