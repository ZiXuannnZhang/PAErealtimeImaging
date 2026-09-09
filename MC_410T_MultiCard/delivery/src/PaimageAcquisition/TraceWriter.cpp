#include "PaimageAcquisition/TraceWriter.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <mutex>
namespace paimage {
namespace {std::mutex registryMutex;std::vector<TraceWriter*> registry;}
TraceWriter::TraceWriter(std::filesystem::path p):TraceWriter(std::move(p),Budget{}){}
TraceWriter::TraceWriter(std::filesystem::path p,Budget b):root_(std::move(p)),budget_(b){
    if(b.records<2||(b.records&(b.records-1))||b.segmentBytes<64||b.totalBytes<64)throw std::invalid_argument("trace budget");
    slots_=std::make_unique<Slot[]>(b.records);for(std::size_t i=0;i<b.records;++i)slots_[i].sequence.store(i);
    worker_=std::thread(&TraceWriter::run,this);
    std::lock_guard<std::mutex> lock(registryMutex);registry.push_back(this);
}
TraceWriter::~TraceWriter(){
    {std::lock_guard<std::mutex> lock(registryMutex);registry.erase(std::remove(registry.begin(),registry.end(),this),registry.end());}
    stop();
}
std::vector<TraceWriter::Cut> TraceWriter::captureCuts(){
    std::lock_guard<std::mutex> lock(registryMutex);std::vector<Cut> cuts;
    for(auto writer:registry){auto progress=writer->exportProgress_;const auto boundary=writer->sequence_.load();const auto position=writer->write_.load();
        auto requested=progress->requested.load();while(requested<position&&!progress->requested.compare_exchange_weak(requested,position)){}
        cuts.push_back({writer->root_,boundary,[progress,position]{
            auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!progress->done&&progress->flushed<position&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return (progress->done||progress->flushed>=position)&&!progress->failed;
        }});
    }return cuts;
}
void TraceWriter::stop(){stopping_.store(true);if(worker_.joinable())worker_.join();}
std::uint64_t TraceWriter::push(TraceRecord r)noexcept{
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
            return r.sequence;
        }
        pos=write_.load(std::memory_order_relaxed);
    }
    dropped_.fetch_add(1,std::memory_order_relaxed);return r.sequence;
}
void TraceWriter::run(){
    std::error_code ec;std::filesystem::create_directories(root_,ec);
    bool failed=bool(ec);unsigned part=0;std::uint64_t bytes=0,segment=0,written=0,unwritten=0;
    std::ofstream file;std::vector<TraceRecord> batch;batch.reserve(4096);
    auto open=[&]{file.open(root_/("trace-"+std::to_string(part++)+".bin"),std::ios::binary|std::ios::trunc);segment=0;failed=!file;};
    if(!failed)open();
    while(true){
        if(exportProgress_->requested>exportProgress_->flushed){
            if(file.is_open())file.flush();
            if(!file||failed)exportProgress_->failed=true;
            exportProgress_->flushed=read_;
        }
        batch.clear();
        while(batch.size()<4096){auto& slot=slots_[read_&(budget_.records-1)];
            if(slot.sequence.load(std::memory_order_acquire)!=read_+1)break;
            batch.push_back(slot.record);slot.sequence.store(read_+budget_.records,std::memory_order_release);++read_;}
        consumed_.store(read_,std::memory_order_relaxed);
        if(batch.empty()) {if(stopping_.load()&&read_==write_.load())break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
        for(std::size_t offset=0;offset<batch.size();){
            if(failed||bytes+64>budget_.totalBytes){unwritten+=batch.size()-offset;writeFailed_.store(true);break;}
            if(segment+64>budget_.segmentBytes){file.close();if(file.fail()){failed=true;continue;}open();if(failed)continue;}
            auto count=std::min<std::uint64_t>(batch.size()-offset,std::min((budget_.segmentBytes-segment)/64,(budget_.totalBytes-bytes)/64));
            file.write(reinterpret_cast<const char*>(batch.data()+offset),std::streamsize(count*64));
            if(!file){failed=true;writeFailed_.store(true);unwritten+=batch.size()-offset;break;}
            offset+=count;written+=count;segment+=count*64;bytes+=count*64;
        }
    }
    if(file.is_open()){file.flush();if(!file)failed=true;file.close();if(file.fail())failed=true;}
    writeFailed_.store(writeFailed_.load()||failed);
    // Summary is committed only after all producers stop and the queue drains.
    // If this file is missing, the analyzer must report unknown/incomplete.
    std::ofstream summary(root_/"trace-summary.json");
    summary<<"{\"schemaVersion\":2,\"recordBytes\":64,\"recordsIssued\":"<<sequence_.load()
        <<",\"recordsWritten\":"<<written<<",\"queueDropped\":"<<dropped_.load()
        <<",\"writerUnwritten\":"<<unwritten<<",\"traceIncomplete\":"<<(incomplete()?"true":"false")
        <<",\"queuePeak\":"<<peak_.load()<<",\"queueAllocatedBytes\":"<<budget_.records*sizeof(Slot)
        <<",\"segmentBudgetBytes\":"<<budget_.segmentBytes<<",\"totalBudgetBytes\":"<<budget_.totalBytes
        <<",\"writeFailed\":"<<(failed?"true":"false")<<"}\n";
    summary.flush();if(!summary)writeFailed_.store(true);
    exportProgress_->failed=incomplete();exportProgress_->done=true;
}
}
