#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <deque>
#include <thread>
#include <vector>

namespace paimage {

enum class LoopKind : std::uint16_t {
    Loop=1, Drain=2, RecvFailure=3, SocketSetup=4, ThreadStart=5,
    BurstMark=6, Stalled=7, ControlMark=8
};

// Fixed 80-byte binary record written by the receive thread and low-rate
// monitoring producers. Fields are kind-specific; the writer thread appends
// records to looplog-N.bin and never blocks a producer.
struct LoopRecord {
    std::uint64_t sequence=0;   // assigned by push(), starts at 1
    std::uint64_t timeNs=0;     // steady-clock event time
    std::uint64_t spanNs=0;     // interval covered by this record
    std::uint64_t session=0;    // measurement session fence
    std::uint32_t threadId=0;
    std::uint16_t kind=0, flags=0;
    std::uint32_t value0=0, value1=0, value2=0, value3=0;
    std::uint64_t payloadA=0, payloadB=0, payloadC=0;
};
static_assert(sizeof(LoopRecord)==80,"fixed loop log ABI");

// Bounded MPSC loop stream with a background writer, mirroring TraceWriter.
// The receive thread only writes fixed-size records into preallocated slots;
// no JSON, file I/O or system calls run on that path.
class LoopLog {
public:
    struct Budget {
        std::size_t records=1<<19;
        std::uint64_t segmentBytes=8*1024*1024;
        std::uint64_t totalBytes=256ull*1024*1024;
        std::uint64_t segmentDurationNs=1000000000ull;
        std::uint64_t retainDurationNs=5000000000ull;
        std::size_t maxFrozenWindows=8;
    };
    explicit LoopLog(std::filesystem::path);
    LoopLog(std::filesystem::path,Budget);
    ~LoopLog();
    std::uint64_t push(LoopRecord) noexcept;
    void stop(); // callers must stop producers first
    // Burst detection: call from the receive thread for every sampling
    // datagram (all acquisition data ports combined; ACKs are not samples).
    // The first sample after >=2 s without samples pushes a BurstMark and
    // bumps the freeze epoch. It only marks a receive-burst window; it never
    // changes session handling, assembly cleanup or round attribution.
    void noteSampleData(std::uint64_t nowNs) noexcept;
    void noteThreadStart(std::uint64_t nowNs) noexcept;
    bool incomplete()const{return dropped_.load()!=0||retentionEvicted_.load()!=0||freezeRejected_.load()!=0||
        exportTruncated_.load()||ioWriteFailed_.load()||budgetExhausted_.load();}
    std::uint64_t dropped()const{return dropped_.load();}
    std::uint64_t retentionEvicted()const{return retentionEvicted_.load();}
    std::uint64_t freezeRejected()const{return freezeRejected_.load();}
    std::uint64_t exportTruncated()const{return exportTruncated_.load();}
    std::uint64_t ioWriteFailed()const{return ioWriteFailed_.load();}
    bool budgetExhausted()const{return budgetExhausted_.load();}
    std::uint64_t freezeEpoch()const{return freezeEpoch_.load();}
    std::uint64_t lastProgressNs()const{return lastProgress_.load();}
    std::uint64_t recordsIssued()const{return sequence_.load();}
    struct Cut {std::filesystem::path root;std::uint64_t sequence=0,dropped=0,freezeEpoch=0;
        std::uint64_t retentionEvicted=0,freezeRejected=0,exportTruncated=0;
        bool incomplete=false,budgetExhausted=false,writeFailed=false;std::function<bool()> flush;};
    static std::vector<Cut> captureCuts();
private:
    struct ExportProgress {std::atomic<std::size_t> requested{0},flushed{0};std::atomic<bool> failed{false},done{false};};
    std::shared_ptr<ExportProgress> exportProgress_=std::make_shared<ExportProgress>();
    struct alignas(64) Slot {std::atomic<std::size_t> sequence;LoopRecord record;};
    void run();
    std::filesystem::path root_;Budget budget_;
    std::unique_ptr<Slot[]> slots_;
    std::atomic<std::size_t> write_{0};std::size_t read_=0;
    std::atomic<std::uint64_t> sequence_{0},dropped_{0},peak_{0},consumed_{0};
    std::atomic<std::uint64_t> retentionEvicted_{0},freezeRejected_{0},exportTruncated_{0};
    std::atomic<std::uint64_t> ioWriteFailed_{0};
    // Compatibility alias for existing callers; it mirrors ioWriteFailed.
    std::atomic<bool> writeFailed_{false};
    std::atomic<bool> stopping_{false},budgetExhausted_{false};
    std::thread worker_;
    std::uint64_t lastSampleNs_=0;std::atomic<std::uint64_t> freezeEpoch_{0},lastProgress_{0};
    std::int64_t qpcStart_=0,qpcFrequency_=0,utcStart100ns_=0;
};

}
