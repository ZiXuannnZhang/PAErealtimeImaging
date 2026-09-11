#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "PaimageAcquisition/LoopLog.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace paimage {
namespace {
std::mutex registryMutex;std::vector<LoopLog*> registry;
std::int64_t qpc() { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v.QuadPart; }
std::int64_t qpf() { LARGE_INTEGER v{}; QueryPerformanceFrequency(&v); return v.QuadPart; }
std::int64_t utc100ns() { FILETIME f{}; GetSystemTimePreciseAsFileTime(&f); ULARGE_INTEGER v{}; v.LowPart=f.dwLowDateTime; v.HighPart=f.dwHighDateTime; return std::int64_t(v.QuadPart); }
}

LoopLog::LoopLog(std::filesystem::path p):LoopLog(std::move(p),Budget{}){}
LoopLog::LoopLog(std::filesystem::path p,Budget b):root_(std::move(p)),budget_(b),qpcStart_(qpc()),qpcFrequency_(qpf()),utcStart100ns_(utc100ns()){
    if(b.records<2||(b.records&(b.records-1))||b.segmentBytes<80||b.totalBytes<80)throw std::invalid_argument("loop log budget");
    slots_=std::make_unique<Slot[]>(b.records);for(std::size_t i=0;i<b.records;++i)slots_[i].sequence.store(i);
    worker_=std::thread(&LoopLog::run,this);
    std::lock_guard<std::mutex> lock(registryMutex);registry.push_back(this);
}
LoopLog::~LoopLog(){
    {std::lock_guard<std::mutex> lock(registryMutex);registry.erase(std::remove(registry.begin(),registry.end(),this),registry.end());}
    stop();
}
std::vector<LoopLog::Cut> LoopLog::captureCuts(){
    std::lock_guard<std::mutex> lock(registryMutex);std::vector<Cut> cuts;
    for(auto log:registry){auto progress=log->exportProgress_;const auto boundary=log->sequence_.load();const auto position=log->write_.load();
        auto requested=progress->requested.load();while(requested<position&&!progress->requested.compare_exchange_weak(requested,position)){}
        cuts.push_back({log->root_,boundary,log->dropped_.load(),log->freezeEpoch_.load(),
            log->retentionEvicted_.load(),log->freezeRejected_.load(),log->exportTruncated_.load(),log->incomplete(),
            log->budgetExhausted_.load(),log->ioWriteFailed_.load()!=0,[progress,position]{auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!progress->done&&progress->flushed<position&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return (progress->done||progress->flushed>=position)&&!progress->failed;}});}
    return cuts;
}
void LoopLog::stop(){stopping_.store(true);if(worker_.joinable())worker_.join();}
std::uint64_t LoopLog::push(LoopRecord r)noexcept{
    r.sequence=sequence_.fetch_add(1)+1;
    auto pos=write_.load(std::memory_order_relaxed);
    for(int attempt=0;attempt<8;++attempt){
        auto& slot=slots_[pos&(budget_.records-1)];auto seq=slot.sequence.load(std::memory_order_acquire);
        auto delta=std::intptr_t(seq)-std::intptr_t(pos);
        if(delta<0)break;
        if(delta==0&&write_.compare_exchange_weak(pos,pos+1,std::memory_order_relaxed)){
            slot.record=r;slot.sequence.store(pos+1,std::memory_order_release);
            auto used=pos+1-consumed_.load(std::memory_order_relaxed);auto peak=peak_.load();
            if(used>peak)peak_.compare_exchange_strong(peak,used);
            if(r.kind==std::uint16_t(LoopKind::Loop))lastProgress_.store(r.timeNs,std::memory_order_relaxed);
            return r.sequence;
        }
        pos=write_.load(std::memory_order_relaxed);
    }
    dropped_.fetch_add(1,std::memory_order_relaxed);return r.sequence;
}
void LoopLog::noteThreadStart(std::uint64_t now)noexcept{lastSampleNs_=now;}
void LoopLog::noteSampleData(std::uint64_t now)noexcept{
    const auto previous=lastSampleNs_;
    lastSampleNs_=now;
    if(!previous||now<previous||now-previous<2ull*1000000000)return;
    const auto epoch=freezeEpoch_.fetch_add(1,std::memory_order_acq_rel)+1;
    LoopRecord r;r.timeNs=now;r.kind=std::uint16_t(LoopKind::BurstMark);
    r.threadId=GetCurrentThreadId();r.value0=std::uint32_t(epoch);r.value1=std::uint32_t(epoch>>32);
    r.spanNs=now-previous;r.payloadA=previous;r.payloadB=now-2ull*1000000000;
    push(r);
}
void LoopLog::run(){
    std::error_code ec;
    std::filesystem::create_directories(root_,ec);
    bool failed=bool(ec);
    if(failed)ioWriteFailed_.fetch_add(1,std::memory_order_relaxed);
    unsigned part=0;
    std::uint64_t segment=0,written=0,unwritten=0;
    std::ofstream file;
    std::filesystem::path currentPath;
    std::vector<LoopRecord> batch;batch.reserve(4096);
    struct Segment {std::filesystem::path path;std::uint64_t first=0,last=0,bytes=0;bool frozen=false;};
    std::deque<Segment> segments;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> frozenWindows;
    std::uint64_t currentFirst=0,currentLast=0,retainedBytes=0;

    auto markIoFailure=[&]{
        if(!failed)ioWriteFailed_.fetch_add(1,std::memory_order_relaxed);
        failed=true;
    };
    auto overlaps=[](std::uint64_t a0,std::uint64_t a1,std::uint64_t b0,std::uint64_t b1){return a0<=b1&&b0<=a1;};
    auto refreshFrozen=[&]{
        for(auto& segmentInfo:segments){
            for(const auto& window:frozenWindows)
                if(overlaps(segmentInfo.first,segmentInfo.last,window.first,window.second)){segmentInfo.frozen=true;break;}
        }
    };
    auto protectEvent=[&](std::uint64_t at){
        const auto before=at>budget_.retainDurationNs?at-budget_.retainDurationNs:0;
        const auto after=(std::numeric_limits<std::uint64_t>::max()-at<budget_.retainDurationNs)
            ?std::numeric_limits<std::uint64_t>::max():at+budget_.retainDurationNs;
        for(const auto& window:frozenWindows)if(overlaps(before,after,window.first,window.second)){refreshFrozen();return;}
        if(frozenWindows.size()>=budget_.maxFrozenWindows){freezeRejected_.fetch_add(1,std::memory_order_relaxed);return;}
        frozenWindows.emplace_back(before,after);refreshFrozen();
    };
    auto closeCurrent=[&]{
        if(!file.is_open())return;
        file.flush();if(!file)markIoFailure();
        file.close();if(file.fail())markIoFailure();
        if(segment){
            bool frozen=false;for(const auto& window:frozenWindows)
                if(overlaps(currentFirst,currentLast,window.first,window.second)){frozen=true;break;}
            segments.push_back({currentPath,currentFirst,currentLast,segment,frozen});
        }
        segment=0;currentFirst=0;currentLast=0;currentPath.clear();
    };
    auto open=[&]{
        currentPath=root_/("looplog-"+std::to_string(part++)+".bin");
        file.open(currentPath,std::ios::binary|std::ios::trunc);
        if(!file){markIoFailure();return false;}
        segment=0;currentFirst=0;currentLast=0;return true;
    };
    auto evictFor=[&](std::uint64_t needed){
        const bool exportHeld=exportProgress_->requested.load(std::memory_order_acquire)>
            exportProgress_->flushed.load(std::memory_order_acquire);
        while(retainedBytes+needed>budget_.totalBytes){
            auto candidate=segments.end();
            if(!exportHeld)for(auto it=segments.begin();it!=segments.end();++it)if(!it->frozen){candidate=it;break;}
            if(candidate==segments.end())return false;
            std::error_code removeEc;std::filesystem::remove(candidate->path,removeEc);
            if(removeEc)return false;
            retainedBytes-=candidate->bytes;
            retentionEvicted_.fetch_add(1,std::memory_order_relaxed);
            segments.erase(candidate);
        }
        return true;
    };
    if(!failed)open();
    while(true){
        if(exportProgress_->requested.load(std::memory_order_acquire)>
           exportProgress_->flushed.load(std::memory_order_acquire)){
            if(file.is_open())file.flush();
            if(!file||failed)exportProgress_->failed=true;
            exportProgress_->flushed=read_;
        }
        batch.clear();
        while(batch.size()<4096){auto& slot=slots_[read_&(budget_.records-1)];
            if(slot.sequence.load(std::memory_order_acquire)!=read_+1)break;
            batch.push_back(slot.record);slot.sequence.store(read_+budget_.records,std::memory_order_release);++read_;}
        consumed_.store(read_,std::memory_order_relaxed);
        if(batch.empty()){if(stopping_.load()&&read_==write_.load())break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
        for(std::size_t offset=0;offset<batch.size();++offset){
            const auto& record=batch[offset];
            if(record.kind==std::uint16_t(LoopKind::BurstMark)||record.kind==std::uint16_t(LoopKind::RecvFailure)||
               record.kind==std::uint16_t(LoopKind::Stalled))protectEvent(record.timeNs);
            if(failed||budgetExhausted_.load()){
                unwritten+=batch.size()-offset;break;
            }
            if(segment&&((segment+sizeof(LoopRecord)>budget_.segmentBytes)||
                (currentFirst&&record.timeNs>=currentFirst+budget_.segmentDurationNs))){
                closeCurrent();
                if(failed||!open()){unwritten+=batch.size()-offset;break;}
            }
            if(!evictFor(sizeof(LoopRecord))){
                budgetExhausted_.store(true,std::memory_order_release);
                if(exportProgress_->requested.load(std::memory_order_acquire)>
                   exportProgress_->flushed.load(std::memory_order_acquire))exportTruncated_.fetch_add(1,std::memory_order_relaxed);
                unwritten+=batch.size()-offset;break;
            }
            file.write(reinterpret_cast<const char*>(&record),std::streamsize(sizeof(record)));
            if(!file){markIoFailure();unwritten+=batch.size()-offset;break;}
            if(!currentFirst)currentFirst=record.timeNs;
            currentLast=record.timeNs;segment+=sizeof(record);retainedBytes+=sizeof(record);++written;
        }
    }
    closeCurrent();
    writeFailed_.store(ioWriteFailed_.load()!=0,std::memory_order_release);
    const auto qpcEnd=qpc(),utcEnd=utc100ns();
    std::ofstream s(root_/"looplog-summary.json");
    s<<"{\"schemaVersion\":1,\"recordBytes\":80,\"recordsIssued\":"<<sequence_.load()
        <<",\"recordsWritten\":"<<written<<",\"queueDropped\":"<<dropped_.load()
        <<",\"retentionEvicted\":"<<retentionEvicted_.load()
        <<",\"freezeRejected\":"<<freezeRejected_.load()
        <<",\"exportTruncated\":"<<exportTruncated_.load()
        <<",\"writerUnwritten\":"<<unwritten<<",\"loopLogIncomplete\":"<<(incomplete()?"true":"false")
        <<",\"queuePeak\":"<<peak_.load()<<",\"queueAllocatedBytes\":"<<budget_.records*sizeof(Slot)
        <<",\"segmentBudgetBytes\":"<<budget_.segmentBytes<<",\"segmentDurationNs\":"<<budget_.segmentDurationNs
        <<",\"retainDurationNs\":"<<budget_.retainDurationNs<<",\"totalBudgetBytes\":"<<budget_.totalBytes
        <<",\"budgetExhausted\":"<<(budgetExhausted_.load()?"true":"false")
        <<",\"ioWriteFailed\":"<<ioWriteFailed_.load()
        <<",\"writeFailed\":"<<(ioWriteFailed_.load() ? "true":"false")
        <<",\"burstMarkEpochs\":"<<freezeEpoch_.load()
        <<",\"qpcFrequency\":"<<qpcFrequency_
        <<",\"anchors\":[{\"qpc\":"<<qpcStart_<<",\"utcFileTime100ns\":"<<utcStart100ns_<<"},{\"qpc\":"<<qpcEnd<<",\"utcFileTime100ns\":"<<utcEnd<<"}]}\n";
    s.flush();if(!s){ioWriteFailed_.fetch_add(1,std::memory_order_relaxed);writeFailed_.store(true);}
    exportProgress_->failed=incomplete();exportProgress_->done=true;
}

}
