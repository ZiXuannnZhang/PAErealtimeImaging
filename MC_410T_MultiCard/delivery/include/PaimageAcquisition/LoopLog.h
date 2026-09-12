#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace paimage {

enum class LoopKind : std::uint16_t {
    Loop=1, Drain=2, RecvFailure=3, SocketSetup=4, ThreadStart=5,
    BurstMark=6, Stalled=7, ControlMark=8
};

// Fixed 80-byte binary record written by the receive thread and low-rate
// monitoring producers. Fields are kind-specific; the producer never does
// JSON formatting, file I/O, or system calls on this path.
struct LoopRecord {
    std::uint64_t sequence=0;
    std::uint64_t timeNs=0;
    std::uint64_t spanNs=0;
    std::uint64_t session=0;
    std::uint32_t threadId=0;
    std::uint16_t kind=0, flags=0;
    std::uint32_t value0=0, value1=0, value2=0, value3=0;
    std::uint64_t payloadA=0, payloadB=0, payloadC=0;
};
static_assert(sizeof(LoopRecord)==80,"fixed loop log ABI");

// A bounded MPSC producer queue feeding a rolling in-memory window worker.
// The worker retains at most 64 MiB of recent records and persists records
// only after a receive-burst marker opens a [burst-5 s, burst+5 s] window.
// With no burst, the listener loop stream is not continuously written; only
// the summary and low-rate lifecycle/aggregate evidence remain.
class LoopLog {
public:
    struct Budget {
        std::size_t records=1<<19;
        std::uint64_t segmentBytes=16*1024*1024;
        std::uint64_t totalBytes=256ull*1024*1024;
    };
    explicit LoopLog(std::filesystem::path);
    LoopLog(std::filesystem::path,Budget);
    ~LoopLog();
    std::uint64_t push(LoopRecord) noexcept;
    void stop(); // callers must stop producers first

    // Called from the receive thread for every acquisition sample datagram.
    // The first sample after >=2 s without samples opens a diagnostic window;
    // it does not alter session handling, assembly cleanup, or attribution.
    void noteSampleData(std::uint64_t nowNs) noexcept;
    void noteThreadStart(std::uint64_t nowNs) noexcept;

    bool incomplete()const{return dropped_.load()!=0||writeFailed_.load()||budgetExhausted_.load();}
    std::uint64_t dropped()const{return dropped_.load();}
    std::uint64_t freezeEpoch()const{return freezeEpoch_.load();}
    std::uint64_t lastProgressNs()const{return lastProgress_.load();}
    std::uint64_t recordsIssued()const{return sequence_.load();}

    struct Cut {
        std::filesystem::path root;
        std::uint64_t sequence=0,dropped=0,freezeEpoch=0;
        bool incomplete=false,budgetExhausted=false,writeFailed=false;
        std::function<bool()> flush;
    };
    static std::vector<Cut> captureCuts();

private:
    struct ExportProgress {
        std::atomic<std::size_t> requested{0},flushed{0};
        std::atomic<bool> failed{false},done{false};
    };
    struct alignas(64) Slot {std::atomic<std::size_t> sequence;LoopRecord record;};
    struct WindowMeta {
        std::uint64_t id=0,startNs=0,endNs=0,recordCount=0,bytes=0;
        std::vector<std::uint64_t> burstEpochs;
        bool preWindowTruncated=false,beforeListenerStart=false;
        bool postWindowTruncated=false,windowTruncated=false,budgetOmitted=false;
        std::uint64_t queueDroppedAtOpen=0,retentionEvictedAtOpen=0;
        std::string file;
    };
    struct ActiveWindow {WindowMeta meta;std::vector<LoopRecord> records;};

    void run();
    void consumeRecord(const LoopRecord&);
    void openWindow(const LoopRecord&);
    void appendToWindow(const LoopRecord&);
    void finalizeWindow(bool force);
    bool writeWindow(const ActiveWindow&);
    void trimRolling(std::uint64_t referenceNs);
    void writeSummary(std::uint64_t recordsWritten,std::uint64_t writerUnwritten);

    std::filesystem::path root_;
    Budget budget_;
    std::unique_ptr<Slot[]> slots_;
    std::atomic<std::size_t> write_{0};
    std::size_t read_=0;
    std::atomic<std::uint64_t> sequence_{0},dropped_{0},peak_{0},consumed_{0};
    std::atomic<bool> stopping_{false},writeFailed_{false},budgetExhausted_{false};
    std::thread worker_;
    std::atomic<std::uint64_t> lastSampleNs_{0},listenerStartNs_{0};
    std::atomic<std::uint64_t> freezeEpoch_{0},lastProgress_{0};
    std::int64_t qpcStart_=0,qpcFrequency_=0,utcStart100ns_=0;
    std::shared_ptr<ExportProgress> exportProgress_=std::make_shared<ExportProgress>();

    static constexpr std::uint64_t kPreWindowNs=5ull*1000000000ull;
    static constexpr std::uint64_t kPostWindowNs=5ull*1000000000ull;
    static constexpr std::uint64_t kWindowBytes=64ull*1024*1024;
    static constexpr std::uint64_t kFrozenBytes=256ull*1024*1024;
    static constexpr std::size_t kRecordBytes=sizeof(LoopRecord);
    std::deque<LoopRecord> rolling_;
    std::optional<ActiveWindow> activeWindow_;
    std::vector<WindowMeta> completedWindows_;
    std::uint64_t rollingBytes_=0,frozenBytes_=0,retentionEvicted_=0;
    std::uint64_t budgetOmittedRecords_=0,pendingRecords_=0,windowWriteRecords_=0;
    std::uint64_t nextWindowId_=1;
    std::uint64_t lastConsumedNs_=0;
    bool listenerStartSeen_=false;
    std::mutex registryMutex_; // retained for ABI/source compatibility; registry uses its own mutex
};

}
