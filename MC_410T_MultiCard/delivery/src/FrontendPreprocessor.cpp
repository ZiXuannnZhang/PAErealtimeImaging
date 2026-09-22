#include "FrontendPreprocessor.h"

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <utility>

namespace {
void updateMax(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}
} // namespace

const char* frontendSubmitResultName(FrontendSubmitResult result) noexcept {
    switch (result) {
    case FrontendSubmitResult::Accepted:    return "Accepted";
    case FrontendSubmitResult::QueueFull:   return "QueueFull";
    case FrontendSubmitResult::QueueBusy:   return "QueueBusy";
    case FrontendSubmitResult::Stopping:    return "Stopping";
    case FrontendSubmitResult::InvalidFrame: return "InvalidFrame";
    case FrontendSubmitResult::StaleSession: return "StaleSession";
    }
    return "InvalidFrame";
}

FrontendPreprocessor::FrontendPreprocessor(int cardId, std::size_t queueCapacity)
    : cardId_(cardId), capacity_(std::max<std::size_t>(1, queueCapacity)) {
    // 出厂配置（任务 3.4 默认值）在构造时设计一次并缓存。这不是一次"重新设计"，
    // 因此 filterDesigns / filterConfigVersion 从 0 起算（见 setFilterConfig）。
    filterConfig_ = frontend_filter::Config{};
    filterBank_ = std::make_shared<const frontend_filter::Bank>(filterConfig_);
}

FrontendPreprocessor::~FrontendPreprocessor() {
    stop();
}

void FrontendPreprocessor::setRingSink(RingSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    ringSink_ = std::move(sink);
}

void FrontendPreprocessor::setDispatchObserver(DispatchObserver observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatchObserver_ = std::move(observer);
}

bool FrontendPreprocessor::setFilterConfig(const frontend_filter::Config& config) {
    if (frontend_filter::validate(config) != frontend_filter::Validation::Ok)
        return false;
    // 先在锁外把新系数完整设计好：SOS 只在参数变更时设计一次并缓存，绝不在每帧
    // 处理时重新设计。设计失败（数值异常）时保持原配置与原系数不变。
    std::shared_ptr<const frontend_filter::Bank> designed;
    try {
        designed = std::make_shared<const frontend_filter::Bank>(config);
    } catch (...) {
        return false;
    }
    std::lock_guard<std::mutex> lock(filterMutex_);
    // 同一份参数重复下发不算一次配置变更，不重复设计也不递增版本号。
    if (filterBank_ && filterConfig_ == config) return true;
    filterBank_ = std::move(designed);
    filterConfig_ = config;
    filterDesigns_.fetch_add(1, std::memory_order_relaxed);
    filterConfigVersion_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

frontend_filter::Config FrontendPreprocessor::filterConfig() const {
    std::lock_guard<std::mutex> lock(filterMutex_);
    return filterConfig_;
}

#ifdef FRONTEND_PREPROCESSOR_TEST_SEAM
void FrontendPreprocessor::setWorkerSeamForTest(std::function<void()> seam) {
    std::lock_guard<std::mutex> lock(mutex_);
    workerSeam_ = std::move(seam);
}

void FrontendPreprocessor::setProcessFaultForTest(
    std::function<void(TriggerGroup&)> fault) {
    std::lock_guard<std::mutex> lock(mutex_);
    processFault_ = std::move(fault);
}

void FrontendPreprocessor::setFilterFaultForTest(
    std::function<void(TriggerGroup&)> fault) {
    std::lock_guard<std::mutex> lock(mutex_);
    filterFault_ = std::move(fault);
}
#endif

void FrontendPreprocessor::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stopping_.store(false, std::memory_order_release);
    try {
        worker_ = std::thread(&FrontendPreprocessor::workerLoop, this);
    } catch (...) {
        stopping_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        throw;
    }
}

void FrontendPreprocessor::stop() {
    if (!running_.exchange(false)) {
        // A stage that was never started still must not hold stale queued work.
        std::lock_guard<std::mutex> lock(mutex_);
        purgeLocked(queue_.size());
        return;
    }
    stopping_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        purgeLocked(queue_.size());
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void FrontendPreprocessor::beginSession(std::uint64_t measurementSession) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Frames already queued or in flight belong to the previous measurement
    // session and must not reach Display/Ring after the session advanced.
    purgeLocked(queue_.size());
    activeSession_ = measurementSession;
    minDispatchableRoundGeneration_ = 0;
    sessionClosed_ = false;
}

void FrontendPreprocessor::endSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    purgeLocked(queue_.size());
    activeSession_ = 0;
    minDispatchableRoundGeneration_ = 0;
    sessionClosed_ = true;
}

void FrontendPreprocessor::advanceRoundBarrier(std::uint64_t measurementSession,
                                               std::uint64_t roundGeneration) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessionClosed_) return;
    if (measurementSession != 0 && activeSession_ != 0 &&
        measurementSession != activeSession_)
        return;
    if (roundGeneration <= minDispatchableRoundGeneration_) return;
    minDispatchableRoundGeneration_ = roundGeneration;
    // Queued frames of the superseded roundGeneration are dropped here so an
    // in-flight frame can still be caught by the pre-dispatch stale check.
    std::size_t removed = 0;
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->raw && staleLocked(*it->raw)) {
            it = queue_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed) {
        depth_.fetch_sub(removed, std::memory_order_relaxed);
        staleDropped_.fetch_add(removed, std::memory_order_relaxed);
    }
}

void FrontendPreprocessor::purgeLocked(std::uint64_t count) noexcept {
    if (count == 0) { queue_.clear(); return; }
    queue_.clear();
    depth_.store(0, std::memory_order_relaxed);
    staleDropped_.fetch_add(count, std::memory_order_relaxed);
}

void FrontendPreprocessor::countReject(FrontendSubmitResult result) noexcept {
    switch (result) {
    case FrontendSubmitResult::QueueFull:
    case FrontendSubmitResult::QueueBusy:
        queueRejected_.fetch_add(1, std::memory_order_relaxed);
        break;
    case FrontendSubmitResult::StaleSession:
        staleDropped_.fetch_add(1, std::memory_order_relaxed);
        break;
    case FrontendSubmitResult::InvalidFrame:
    case FrontendSubmitResult::Stopping:
    case FrontendSubmitResult::Accepted:
        break;
    }
}

bool FrontendPreprocessor::staleLocked(const TriggerGroup& group) const {
    if (sessionClosed_ || stopping_.load(std::memory_order_acquire)) return true;
    const std::uint64_t active = activeSession_;
    if (active != 0 && group.measurementSession != 0 &&
        group.measurementSession != active)
        return true;
    const std::uint64_t minRound = minDispatchableRoundGeneration_;
    if (group.roundGeneration < minRound) {
        // CountBoundary contract: the final logical trigger of the round the
        // barrier just closed must still be dispatched exactly once.
        const bool justCompletedFinal = group.isFinalLogicalTrigger &&
                                        group.roundGeneration + 1 == minRound;
        if (!justCompletedFinal) return true;
    }
    return false;
}

FrontendSubmitResult FrontendPreprocessor::submit(const TriggerGroupPtr& raw) noexcept {
    if (!raw || raw->sampleCount <= 0 || raw->freqA.empty() || raw->freqB.empty()) {
        countReject(FrontendSubmitResult::InvalidFrame);
        return FrontendSubmitResult::InvalidFrame;
    }
    if (stopping_.load(std::memory_order_acquire)) {
        countReject(FrontendSubmitResult::Stopping);
        return FrontendSubmitResult::Stopping;
    }
    // Non-blocking with respect to the worker: the queue lock is only held for
    // the O(1) enqueue and is never held across the deep copy, display
    // preparation or Ring dispatch.  A try-lock admission would reject frames
    // on mere lock contention, which would drop real A-lines from the Ring for
    // no capacity reason; contention here is therefore resolved in bounded time
    // instead of being turned into a drop.
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_.load(std::memory_order_acquire)) {
        countReject(FrontendSubmitResult::Stopping);
        return FrontendSubmitResult::Stopping;
    }
    if (staleLocked(*raw)) {
        countReject(FrontendSubmitResult::StaleSession);
        return FrontendSubmitResult::StaleSession;
    }
    if (queue_.size() >= capacity_) {
        countReject(FrontendSubmitResult::QueueFull);
        return FrontendSubmitResult::QueueFull;
    }
    queue_.push_back(Item{raw});
    const auto depth = depth_.fetch_add(1, std::memory_order_relaxed) + 1;
    updateMax(maxDepth_, depth);
    accepted_.fetch_add(1, std::memory_order_relaxed);
    lock.unlock();
    ready_.notify_one();
    return FrontendSubmitResult::Accepted;
}

// 载荷语义核实（2026-09-23）：freqA/freqB 是时域幅度波形，不是瞬时频率（kHz）。
// 因此滤波按"时域幅度的 A-line"设计，逐 A-line 独立作用于 frontend clone 的
// freqA / freqB，不改写 raw（保存与发布仍消费 raw）。
//
// 唯一滤波插入点：本函数是全仓唯一执行滤波的位置。顺序固定为
// 滤波 -> 显示派生（prepareDisplayData）-> 分发，见 workerLoop。
void FrontendPreprocessor::processFrontendSignal(
    TriggerGroup& frontend, const frontend_filter::Bank& bank) {
    bool didFilter = false;
    // 逐 A-line 独立：一次滤波的输入是一条通道在一个 TriggerGroup 内的完整采样
    // 序列；Bank 不携带任何跨 A-line / 跨触发 / 跨卡的滤波状态。
    if (!frontend.freqA.empty() && bank.apply(frontend.freqA.data(),
                                              frontend.freqA.size()))
        didFilter = true;
    if (!frontend.freqB.empty() && bank.apply(frontend.freqB.data(),
                                              frontend.freqB.size()))
        didFilter = true;
    if (didFilter) filteredFrames_.fetch_add(1, std::memory_order_relaxed);
#ifdef FRONTEND_PREPROCESSOR_TEST_SEAM
    if (filterFault_) filterFault_(frontend);
#endif
}

// 载荷语义核实（2026-09-23）：freqA/freqB 是时域幅度波形，不是瞬时频率（kHz）。
// 完整证据见 include/DataTypes.h 的核实记录（落盘样本零均值双向振荡 + MATLAB
// 参照 preprocessBlock 的 butter/filtfilt + DAS 用法）。
// 因此下面的 phasePerKhz 是 ogprog「kHz→rad」契约的遗留系数：它原本与
// computeFrequency 的 scale 精确配对（scale × phasePerKhz = π/maxIntVal），
// 而 computeFrequency 现为 identity，配对已断，故 phase*_display 目前既非
// 差分相位也非累积相位。该修复本轮搁置，不影响滤波插入点。
// 高/低通零相位滤波仍只在 processFrontendSignal() 作用一次，逐 A-line 独立。
void FrontendPreprocessor::prepareDisplayData(TriggerGroup& group) {
    const int n = std::min(group.sampleCount,
        std::min(static_cast<int>(group.freqA.size()),
                 static_cast<int>(group.freqB.size())));
    if (n <= 0) {
        group.freqA_display.clear();
        group.freqB_display.clear();
        group.phaseA_display.clear();
        group.phaseB_display.clear();
        return;
    }
    // Full-resolution display: every acquired sample is kept.  Display
    // downsampling is deliberately not reintroduced.
    group.freqA_display.assign(group.freqA.begin(), group.freqA.begin() + n);
    group.freqB_display.assign(group.freqB.begin(), group.freqB.begin() + n);
    group.phaseA_display.resize(n);
    group.phaseB_display.resize(n);
    constexpr float phasePerKhz =
        static_cast<float>(2.0 * M_PI * M_PI * 1000.0 / FPGA_ADC_FREQ_HZ);
    float phaseA = 0.0f, phaseB = 0.0f;
    for (int i = 0; i < n; ++i) {
        phaseA += group.freqA[i] * phasePerKhz;
        phaseB += group.freqB[i] * phasePerKhz;
        group.phaseA_display[i] = phaseA;
        group.phaseB_display[i] = phaseB;
    }
}

void FrontendPreprocessor::dispatch(TriggerGroupPtr& frontend) {
    DispatchObserver observer;
    RingSink ring;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        observer = dispatchObserver_;
        ring = ringSink_;
    }
    // Display and Ring consume the same frontend-owned clone.
    if (displayBuffer_) {
        if (observer) observer("display", frontend.get());
        displayBuffer_->update(frontend);
        displayBuffer_->updateFullRes(frontend);
        displayUpdates_.fetch_add(1, std::memory_order_relaxed);
    }
    if (ring) {
        downstreamRingAttempts_.fetch_add(1, std::memory_order_relaxed);
        if (observer) observer("ring", frontend.get());
        try {
            const ImagingSubmitResult result = ring(frontend);
            if (result == ImagingSubmitResult::Accepted)
                downstreamRingAccepted_.fetch_add(1, std::memory_order_relaxed);
            else
                downstreamRingRejected_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            // A downstream callback exception must never escape and must never
            // take down the acquisition/save threads.  It is reported as a
            // downstream result, never as ingress or save loss.
            downstreamRingExceptions_.fetch_add(1, std::memory_order_relaxed);
            downstreamRingRejected_.fetch_add(1, std::memory_order_relaxed);
            throw;
        }
    }
}

void FrontendPreprocessor::workerLoop() noexcept {
    for (;;) {
        Item item;
        // Per-frame frozen coefficients.  Captured at dequeue so a frame already
        // taken by the worker completes on the coefficients it started with,
        // while every frame dequeued after a configuration update uses the new
        // ones (任务 3.6 生效时机).
        std::shared_ptr<const frontend_filter::Bank> frameBank;
#ifdef FRONTEND_PREPROCESSOR_TEST_SEAM
        std::function<void()> seam;
#endif
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] {
                return stopping_.load(std::memory_order_acquire) || !queue_.empty();
            });
            if (stopping_.load(std::memory_order_acquire) && queue_.empty()) break;
            if (queue_.empty()) continue;
            item = std::move(queue_.front());
            queue_.pop_front();
            depth_.fetch_sub(1, std::memory_order_relaxed);
#ifdef FRONTEND_PREPROCESSOR_TEST_SEAM
            seam = workerSeam_;
#endif
        }
        {
            std::lock_guard<std::mutex> lock(filterMutex_);
            frameBank = filterBank_;
        }
        if (!item.raw) continue;

        // Frozen per-frame order (Task 3.3):
        //   dequeue raw const -> freeze coefficients -> stale check -> deep copy
        //   -> processFrontendSignal -> full-resolution display preparation
        //   -> stale check again
        //   -> DisplayBuffer update / updateFullRes -> RingFeedSink
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (staleLocked(*item.raw)) {
                staleDropped_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }
#ifdef FRONTEND_PREPROCESSOR_TEST_SEAM
        if (seam) seam();
#endif
        TriggerGroupPtr frontend;
        try {
            frontend = std::make_shared<TriggerGroup>(*item.raw);
        } catch (const std::bad_alloc&) {
            exceptionDropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        } catch (...) {
            exceptionDropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        deepCopies_.fetch_add(1, std::memory_order_relaxed);
        try {
            if (frameBank) processFrontendSignal(*frontend, *frameBank);
#ifdef FRONTEND_PREPROCESSOR_TEST_SEAM
            if (processFault_) processFault_(*frontend);
#endif
            prepareDisplayData(*frontend);
        } catch (...) {
            // 滤波实现抛出的异常不逃逸出 worker 线程：计入既有 exceptionDropped
            // 并继续处理后续帧。
            exceptionDropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Second stale check: a frame already taken by the worker must still be
        // dropped if a session/timeout boundary moved while it was in flight.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (staleLocked(*frontend)) {
                staleDropped_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }

        try {
            dispatch(frontend);
        } catch (...) {
            exceptionDropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        processed_.fetch_add(1, std::memory_order_relaxed);
    }
}

FrontendPreprocessor::Snapshot FrontendPreprocessor::snapshot() const noexcept {
    Snapshot s;
    s.accepted = accepted_.load(std::memory_order_relaxed);
    s.processed = processed_.load(std::memory_order_relaxed);
    s.queueRejected = queueRejected_.load(std::memory_order_relaxed);
    s.staleDropped = staleDropped_.load(std::memory_order_relaxed);
    s.exceptionDropped = exceptionDropped_.load(std::memory_order_relaxed);
    s.currentDepth = depth_.load(std::memory_order_relaxed);
    s.maxDepth = maxDepth_.load(std::memory_order_relaxed);
    s.downstreamRingAttempts = downstreamRingAttempts_.load(std::memory_order_relaxed);
    s.downstreamRingAccepted = downstreamRingAccepted_.load(std::memory_order_relaxed);
    s.downstreamRingRejected = downstreamRingRejected_.load(std::memory_order_relaxed);
    s.downstreamRingExceptions = downstreamRingExceptions_.load(std::memory_order_relaxed);
    s.displayUpdates = displayUpdates_.load(std::memory_order_relaxed);
    s.deepCopies = deepCopies_.load(std::memory_order_relaxed);
    s.filteredFrames = filteredFrames_.load(std::memory_order_relaxed);
    s.filterDesigns = filterDesigns_.load(std::memory_order_relaxed);
    s.filterConfigVersion = filterConfigVersion_.load(std::memory_order_relaxed);
    s.queueCapacity = capacity_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s.activeSession = activeSession_;
        s.minDispatchableRoundGeneration = minDispatchableRoundGeneration_;
    }
    return s;
}
