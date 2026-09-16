#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <functional>
#include <vector>

namespace paimage {
enum class TimingKind : std::uint16_t {
    ThreadLife=1, Select=2, Drain=3, LoopGap=4, Recvfrom=5, TracePush=6,
    IngressSink=7, CoreMutexWait=8, CoreIngest=9, CorePoll=10,
    ControlMutexWait=11, ControlMutexHold=12, CardEnqueue=13, SyncEnqueue=14,
    CardWorker=15, SyncWorker=16
};
struct TimingRecord {
    std::uint64_t sequence=0,startNs=0,endNs=0,session=0,correlation=0;
    std::uint32_t threadId=0,value0=0,value1=0;
    std::uint16_t localPort=0;std::int16_t card=-1;
    std::uint16_t kind=0,flags=0;std::uint32_t reserved=0;
};
static_assert(sizeof(TimingRecord)==64,"fixed timing ABI");

// Bounded, independent timing stream. observe() always updates fixed counters;
// detailed records are optional so normal intervals remain inexpensive.
class TimingWriter {
public:
    struct Budget {std::size_t records=131072;std::uint64_t segmentBytes=16*1024*1024,totalBytes=256ull*1024*1024;};
    explicit TimingWriter(std::filesystem::path);
    TimingWriter(std::filesystem::path,Budget);
    ~TimingWriter();
    void observe(TimingRecord,bool keepDetail) noexcept;
    void stop();
    bool incomplete()const{return dropped_.load()!=0||writeFailed_.load();}
    std::uint64_t dropped()const{return dropped_.load();}
    struct Cut {std::filesystem::path root;std::uint64_t sequence=0;std::function<bool()> flush;};
    static std::vector<Cut> captureCuts();
private:
    struct ExportProgress {std::atomic<std::size_t> requested{0},flushed{0};std::atomic<bool> failed{false},done{false};};
    std::shared_ptr<ExportProgress> exportProgress_=std::make_shared<ExportProgress>();
    struct Aggregate {std::atomic<std::uint64_t> count{0},totalNs{0},maxNs{0};std::array<std::atomic<std::uint64_t>,5> buckets{};};
    struct alignas(64) Slot {std::atomic<std::size_t> sequence;TimingRecord record;};
    void run();
    std::filesystem::path root_;Budget budget_;std::unique_ptr<Slot[]> slots_;
    std::atomic<std::size_t> write_{0},consumed_{0};std::size_t read_=0;
    std::atomic<std::uint64_t> sequence_{0},dropped_{0},peak_{0};
    std::atomic<bool> stopping_{false},writeFailed_{false};std::thread worker_;
    std::array<Aggregate,32> aggregates_{};
    std::int64_t qpcStart_=0,qpcFrequency_=0,utcStart100ns_=0;
};
}
