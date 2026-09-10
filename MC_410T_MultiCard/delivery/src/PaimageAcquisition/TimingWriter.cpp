#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "PaimageAcquisition/TimingWriter.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <mutex>

namespace paimage {
namespace {
std::mutex registryMutex;std::vector<TimingWriter*> registry;
std::int64_t qpc() { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v.QuadPart; }
std::int64_t qpf() { LARGE_INTEGER v{}; QueryPerformanceFrequency(&v); return v.QuadPart; }
std::int64_t utc100ns() { FILETIME f{}; GetSystemTimePreciseAsFileTime(&f); ULARGE_INTEGER v{}; v.LowPart=f.dwLowDateTime; v.HighPart=f.dwHighDateTime; return std::int64_t(v.QuadPart); }
}
TimingWriter::TimingWriter(std::filesystem::path p):TimingWriter(std::move(p),Budget{}){}
TimingWriter::TimingWriter(std::filesystem::path p,Budget b):root_(std::move(p)),budget_(b),qpcStart_(qpc()),qpcFrequency_(qpf()),utcStart100ns_(utc100ns()){
    if(b.records<2||(b.records&(b.records-1))||b.segmentBytes<64||b.totalBytes<64)throw std::invalid_argument("timing budget");
    slots_=std::make_unique<Slot[]>(b.records);for(std::size_t i=0;i<b.records;++i)slots_[i].sequence.store(i);
    worker_=std::thread(&TimingWriter::run,this);
    std::lock_guard<std::mutex> lock(registryMutex);registry.push_back(this);
}
TimingWriter::~TimingWriter(){{std::lock_guard<std::mutex> lock(registryMutex);registry.erase(std::remove(registry.begin(),registry.end(),this),registry.end());}stop();}
std::vector<TimingWriter::Cut> TimingWriter::captureCuts(){
    std::lock_guard<std::mutex> lock(registryMutex);std::vector<Cut> cuts;
    for(auto writer:registry){auto progress=writer->exportProgress_;const auto boundary=writer->sequence_.load();const auto position=writer->write_.load();auto requested=progress->requested.load();while(requested<position&&!progress->requested.compare_exchange_weak(requested,position)){}
        cuts.push_back({writer->root_,boundary,[progress,position]{auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!progress->done&&progress->flushed<position&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(2));return (progress->done||progress->flushed>=position)&&!progress->failed;}});}
    return cuts;
}
void TimingWriter::stop(){stopping_=true;if(worker_.joinable())worker_.join();}
void TimingWriter::observe(TimingRecord r,bool keep)noexcept{
    const auto duration=r.endNs>=r.startNs?r.endNs-r.startNs:0;const auto kind=std::min<std::size_t>(r.kind,aggregates_.size()-1);auto& a=aggregates_[kind];
    a.count.fetch_add(1,std::memory_order_relaxed);a.totalNs.fetch_add(duration,std::memory_order_relaxed);auto maximum=a.maxNs.load(std::memory_order_relaxed);
    while(duration>maximum&&!a.maxNs.compare_exchange_weak(maximum,duration,std::memory_order_relaxed)){}
    const std::size_t bucket=duration<500000?0:duration<2000000?1:duration<10000000?2:duration<100000000?3:4;
    a.buckets[bucket].fetch_add(1,std::memory_order_relaxed);if(!keep)return;
    r.sequence=sequence_.fetch_add(1)+1;auto pos=write_.load(std::memory_order_relaxed);
    for(int attempt=0;attempt<8;++attempt){auto& slot=slots_[pos&(budget_.records-1)];auto seq=slot.sequence.load(std::memory_order_acquire);auto delta=std::intptr_t(seq)-std::intptr_t(pos);
        if(delta<0)break;
        if(delta==0&&write_.compare_exchange_weak(pos,pos+1,std::memory_order_relaxed)){slot.record=r;slot.sequence.store(pos+1,std::memory_order_release);auto used=pos+1-consumed_.load();auto peak=peak_.load();if(used>peak)peak_.compare_exchange_strong(peak,used);return;}
        pos=write_.load(std::memory_order_relaxed);}
    dropped_.fetch_add(1,std::memory_order_relaxed);
}
void TimingWriter::run(){
    std::error_code ec;std::filesystem::create_directories(root_,ec);bool failed=bool(ec);unsigned part=0;std::uint64_t bytes=0,segment=0,written=0,unwritten=0;
    std::ofstream file;std::vector<TimingRecord> batch;batch.reserve(4096);auto open=[&]{file.open(root_/("timing-"+std::to_string(part++)+".bin"),std::ios::binary|std::ios::trunc);segment=0;failed=!file;};if(!failed)open();
    while(true){
        // read_ here refers only to batches whose writes have already completed.
        // Never acknowledge the batch removed from the queue below before writing it.
        if(exportProgress_->requested>exportProgress_->flushed){if(file.is_open())file.flush();if(!file||failed||incomplete())exportProgress_->failed=true;exportProgress_->flushed=read_;}
        batch.clear();while(batch.size()<4096){auto& slot=slots_[read_&(budget_.records-1)];if(slot.sequence.load(std::memory_order_acquire)!=read_+1)break;batch.push_back(slot.record);slot.sequence.store(read_+budget_.records,std::memory_order_release);++read_;}consumed_=read_;
        if(batch.empty()){if(stopping_&&read_==write_.load())break;std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
        for(std::size_t offset=0;offset<batch.size();){if(failed||bytes+64>budget_.totalBytes){unwritten+=batch.size()-offset;writeFailed_=true;break;}if(segment+64>budget_.segmentBytes){file.close();if(file.fail()){failed=true;continue;}open();if(failed)continue;}auto count=std::min<std::uint64_t>(batch.size()-offset,std::min((budget_.segmentBytes-segment)/64,(budget_.totalBytes-bytes)/64));file.write(reinterpret_cast<const char*>(batch.data()+offset),std::streamsize(count*64));if(!file){failed=true;writeFailed_=true;unwritten+=batch.size()-offset;break;}offset+=count;written+=count;segment+=count*64;bytes+=count*64;}}
    if(file.is_open()){file.flush();if(!file)failed=true;file.close();if(file.fail())failed=true;}writeFailed_=writeFailed_||failed;const auto qpcEnd=qpc(),utcEnd=utc100ns();
    std::ofstream s(root_/"timing-summary.json");s<<"{\"schemaVersion\":1,\"recordBytes\":64,\"recordsIssued\":"<<sequence_.load()<<",\"recordsWritten\":"<<written<<",\"queueDropped\":"<<dropped_.load()<<",\"writerUnwritten\":"<<unwritten<<",\"timingIncomplete\":"<<(incomplete()?"true":"false")<<",\"queuePeak\":"<<peak_.load()<<",\"queueAllocatedBytes\":"<<budget_.records*sizeof(Slot)<<",\"totalBudgetBytes\":"<<budget_.totalBytes<<",\"qpcFrequency\":"<<qpcFrequency_<<",\"anchors\":[{\"qpc\":"<<qpcStart_<<",\"utcFileTime100ns\":"<<utcStart100ns_<<"},{\"qpc\":"<<qpcEnd<<",\"utcFileTime100ns\":"<<utcEnd<<"}],\"aggregates\":{";
    bool first=true;for(std::size_t kind=1;kind<aggregates_.size();++kind){auto count=aggregates_[kind].count.load();if(!count)continue;if(!first)s<<',';first=false;s<<'\"'<<kind<<"\":{\"count\":"<<count<<",\"totalNs\":"<<aggregates_[kind].totalNs.load()<<",\"maxNs\":"<<aggregates_[kind].maxNs.load()<<",\"bucketsLt0_5_2_10_100ms\":[";for(int bucket=0;bucket<5;++bucket){if(bucket)s<<',';s<<aggregates_[kind].buckets[bucket].load();}s<<"]}";}s<<"}}\n";s.flush();if(!s)writeFailed_=true;exportProgress_->failed=incomplete();exportProgress_->done=true;
}
}
