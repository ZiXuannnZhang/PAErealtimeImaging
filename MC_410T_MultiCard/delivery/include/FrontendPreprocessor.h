#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "DataTypes.h"
#include "DisplayBuffer.h"
#include "ImagingBypass.h"

// ============================================================
// Frontend Preprocessing Stage  卡级异步前端预处理
//
// 目标数据链（Task 1 = identity processing）：
//   raw TriggerGroup
//   ├─ FileSaver / raw save      -> raw，不经过本 stage
//   ├─ FramePublisher            -> raw，不经过本 stage
//   └─ FrontendPreprocessor::submit(raw const)
//         ↓ bounded FIFO / worker
//         ↓ deep-copy frontend-owned TriggerGroup
//         ↓ processFrontendSignal()          <- 唯一滤波插入点（本任务 identity）
//         ↓ full-resolution display preparation
//         ├─ DisplayBuffer::update / updateFullRes
//         └─ RingFeedSink / ImagingBypass
//
// Display 与 Ring 消费同一次 preprocessing 产生的同一个 frontend-owned clone。
// 热路径（HostOutput / DataProcessor）只做 enqueue：不做 deep copy、不做 display
// preparation、不调用 Ring consumer。
// ============================================================

// Frontend queue admission result.  This deliberately does NOT reuse
// ImagingSubmitResult: it only describes enqueue into the per-card frontend
// stage and never claims to be the asynchronous downstream Ring outcome.
// Ring Accepted/QueueFull/Busy/Disabled continues to be owned by
// ImagingBypass/Ring statistics.
enum class FrontendSubmitResult : std::uint8_t {
    Accepted = 0,
    // The bounded FIFO is full: this is the only admission rejection that
    // reflects real capacity pressure.
    QueueFull,
    // Reserved for a try-lock admission policy.  The current submit holds the
    // queue lock only for the O(1) enqueue and never produces this value: lock
    // contention must not cost a real A-line.
    QueueBusy,
    Stopping,
    InvalidFrame,
    StaleSession
};

const char* frontendSubmitResultName(FrontendSubmitResult result) noexcept;

class FrontendPreprocessor final {
public:
    // Default keeps ~1.3 s of 200 Hz triggers per card while bounding memory.
    static constexpr std::size_t kDefaultQueueCapacity = 256;

    struct Snapshot {
        // Frontend queue admission (a.k.a. enqueued).
        std::uint64_t accepted = 0;
        std::uint64_t processed = 0;
        std::uint64_t queueRejected = 0;
        std::uint64_t staleDropped = 0;
        std::uint64_t exceptionDropped = 0;
        std::uint64_t currentDepth = 0;
        std::uint64_t maxDepth = 0;
        // Downstream Ring outcomes only.  These are never folded into UDP
        // packet loss, missingTriggerCount or saveQueueDiscards.
        std::uint64_t downstreamRingAttempts = 0;
        std::uint64_t downstreamRingAccepted = 0;
        std::uint64_t downstreamRingRejected = 0;
        std::uint64_t downstreamRingExceptions = 0;
        // Display dispatch count (same clone as the Ring consumer).
        std::uint64_t displayUpdates = 0;
        std::uint64_t deepCopies = 0;
        std::uint64_t activeSession = 0;
        std::uint64_t minDispatchableRoundGeneration = 0;
        std::uint64_t queueCapacity = 0;
    };

    // Existing Ring feed callback shape (MainWindow -> ImagingBypass::tryPush).
    using RingSink = std::function<ImagingSubmitResult(const TriggerGroupConstPtr&)>;
    // Observability hook fired with the frontend-owned clone immediately before
    // each downstream dispatch.  Used to prove one clone feeds both consumers.
    using DispatchObserver =
        std::function<void(const char* consumer, const TriggerGroup* frontendOwned)>;

    explicit FrontendPreprocessor(int cardId,
                                  std::size_t queueCapacity = kDefaultQueueCapacity);
    ~FrontendPreprocessor();

    FrontendPreprocessor(const FrontendPreprocessor&) = delete;
    FrontendPreprocessor& operator=(const FrontendPreprocessor&) = delete;

    void setDisplayBuffer(DisplayBuffer* displayBuffer) noexcept {
        displayBuffer_ = displayBuffer;
    }
    void setRingSink(RingSink sink);
    void setDispatchObserver(DispatchObserver observer);

    // The stage must be running before any producer can submit (Task 3.7).
    void start();
    // Deterministic join; terminate is never used as a normal stop mechanism.
    void stop();

    // Session / boundary lifecycle.  Production wires these to the existing
    // measurement session and PhysicalRoundNormalizer boundary facts; the stage
    // never infers a round of its own.
    void beginSession(std::uint64_t measurementSession);
    void endSession();
    // TimeoutBoundary advances the stale barrier.  CountBoundary does not: the
    // just-completed final logical trigger must still dispatch exactly once and
    // is additionally exempt from the round-generation barrier.
    void advanceRoundBarrier(std::uint64_t measurementSession,
                             std::uint64_t roundGeneration);

    // Non-blocking hot-path entry.  The input is const raw TriggerGroup: the
    // deep copy happens later on the worker thread and the raw object is never
    // mutated here.
    FrontendSubmitResult submit(const TriggerGroupPtr& raw) noexcept;

    Snapshot snapshot() const noexcept;
    int cardId() const noexcept { return cardId_; }

#ifdef FRONTEND_PREPROCESSOR_TEST_SEAM
    // Deterministic test seams.  Not compiled into the production target.
    void setWorkerSeamForTest(std::function<void()> seam);
    void setProcessFaultForTest(std::function<void(TriggerGroup&)> fault);
#endif

private:
    struct Item {
        TriggerGroupPtr raw;
    };

    void workerLoop() noexcept;
    // Must be called with mutex_ held.  Covers session staleness and the
    // round-generation stale barrier (with the just-completed final exemption).
    bool staleLocked(const TriggerGroup& group) const;
    void purgeLocked(std::uint64_t count) noexcept;
    void countReject(FrontendSubmitResult result) noexcept;
    // Not noexcept: a downstream display/Ring fault is rethrown so the worker
    // loop can record it as exceptionDropped and keep serving later frames.
    void dispatch(TriggerGroupPtr& frontend);

    // The single insertion point for future high/low-pass filtering.  Task 1
    // implementation is identity; filtering must act exactly once, here.
    static void processFrontendSignal(TriggerGroup& frontend) noexcept;
    // Full-resolution display preparation: every acquired sample is kept.
    // No display downsampling is reintroduced.
    static void prepareDisplayData(TriggerGroup& frontend);

    const int cardId_;
    const std::size_t capacity_;

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Item> queue_;
    std::thread worker_;
    RingSink ringSink_;
    DispatchObserver dispatchObserver_;
    DisplayBuffer* displayBuffer_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{true};
    // Session/boundary state is read by the worker under mutex_.
    std::uint64_t activeSession_ = 0;
    std::uint64_t minDispatchableRoundGeneration_ = 0;
    bool sessionClosed_ = true;

    std::atomic<std::uint64_t> accepted_{0}, processed_{0}, queueRejected_{0};
    std::atomic<std::uint64_t> staleDropped_{0}, exceptionDropped_{0};
    std::atomic<std::uint64_t> depth_{0}, maxDepth_{0}, deepCopies_{0};
    std::atomic<std::uint64_t> displayUpdates_{0};
    std::atomic<std::uint64_t> downstreamRingAttempts_{0};
    std::atomic<std::uint64_t> downstreamRingAccepted_{0};
    std::atomic<std::uint64_t> downstreamRingRejected_{0};
    std::atomic<std::uint64_t> downstreamRingExceptions_{0};

#ifdef FRONTEND_PREPROCESSOR_TEST_SEAM
    std::function<void()> workerSeam_;
    std::function<void(TriggerGroup&)> processFault_;
#endif
};
