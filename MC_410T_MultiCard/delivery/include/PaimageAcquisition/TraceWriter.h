#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <functional>
#include <vector>

namespace paimage {
struct TraceRecord {
    std::uint64_t sequence=0, monotonicNs=0, session=0, correlation=0;
    std::uint32_t threadId=0, sourceIPv4=0, value=0;
    std::uint16_t localPort=0,sourcePort=0,length=0,trigger=0,packet=0;
    std::int16_t card=-1;
    std::uint8_t stage=0,reason=0,header[4]{};
    std::uint16_t schema=2; // output correlation = frame's first raw ingress ID
};
static_assert(sizeof(TraceRecord)==64,"fixed binary trace ABI");
// Bounded MPSC trace queue. No string formatting, payload copies or file I/O in push.
class TraceWriter {
public:
    struct Budget {std::size_t records=131072;std::uint64_t segmentBytes=64*1024*1024,totalBytes=1024ull*1024*1024;};
    explicit TraceWriter(std::filesystem::path);
    TraceWriter(std::filesystem::path,Budget);
    ~TraceWriter();
    std::uint64_t push(TraceRecord) noexcept;
    void stop(); // callers must stop producers first
    std::uint64_t boundary() const{return sequence_.load();}
    std::uint64_t dropped()const{return dropped_.load();}
    bool incomplete()const{return dropped_.load()!=0||writeFailed_.load();}
    struct Cut {std::filesystem::path root;std::uint64_t sequence=0;std::function<bool()> flush;};
    static std::vector<Cut> captureCuts();
private:
    struct ExportProgress {std::atomic<std::size_t> requested{0},flushed{0};std::atomic<bool> failed{false},done{false};};
    std::shared_ptr<ExportProgress> exportProgress_=std::make_shared<ExportProgress>();
    struct alignas(64) Slot {std::atomic<std::size_t> sequence;TraceRecord record;};
    void run();
    std::filesystem::path root_;Budget budget_;std::unique_ptr<Slot[]> slots_;
    std::atomic<std::size_t> write_{0};std::size_t read_=0;
    std::atomic<std::uint64_t> sequence_{0},dropped_{0},peak_{0};
    std::atomic<std::size_t> consumed_{0};
    std::atomic<bool> stopping_{false},writeFailed_{false};std::thread worker_;
};
}
