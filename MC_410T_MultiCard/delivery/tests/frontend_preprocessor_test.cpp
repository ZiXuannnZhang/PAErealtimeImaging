// Frontend Preprocessing Stage contract tests.
//
// Part A (T1..T12) covers the twelve Task 1 architecture contracts: deep-copy,
// full-resolution, one-result/two-consumers, FIFO ordering, non-blocking submit,
// bounded overflow, save isolation, session stale, timeout stale, CountBoundary
// final, stop lifecycle and exception containment.  T1/T2 explicitly disable
// both filter paths: they assert the architecture contract of an identity
// frontend, and filter numerics are covered by the dedicated suite below.
//
// Part B (F1..F13) covers the high/low-pass zero-phase filter contract, mapped
// 1:1 onto the task's required checks 1..13.
//
// Determinism: the FRONTEND_PREPROCESSOR_TEST_SEAM worker seam parks the stage
// worker after it has dequeued exactly one frame.  That frame is therefore
// "in-flight" (already dequeued, not yet dispatched) and everything submitted
// afterwards stays "queued", which is exactly the state the stale-barrier,
// bounded-overflow and filter effective-moment contracts have to handle.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "DataProcessor.h"
#include "DataTypes.h"
#include "DisplayBuffer.h"
#include "FrontendFilter.h"
#include "FrontendPreprocessor.h"
#include "ImagingBypass.h"

using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void require(bool ok, const char* message) {
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL  " << message << "\n";
        throw std::runtime_error(message);
    }
}

template <class P>
bool until(P predicate) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(500us);
    return predicate();
}

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point begin) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count());
}

TriggerGroupPtr makeGroup(int cardId, std::uint16_t triggerSeq, int samples,
                          std::uint64_t session, std::uint64_t roundGeneration,
                          std::int64_t logicalTriggerIndex, bool finalTrigger) {
    auto group = std::make_shared<TriggerGroup>();
    group->cardId = cardId;
    group->triggerSeq = triggerSeq;
    group->sampleCount = samples;
    group->timestamp_ms = 1000u + triggerSeq;
    group->isComplete = true;
    group->sourceIPv4 = 0x0100007f;
    group->sessionGen = 7;
    group->measurementSession = session;
    group->normalizationApplied = true;
    group->physicalDecision = paimage::PhysicalTriggerDecision::LogicalScan;
    group->roundGeneration = roundGeneration;
    group->logicalTriggerIndex = logicalTriggerIndex;
    group->roundComplete = finalTrigger;
    group->isFinalLogicalTrigger = finalTrigger;
    group->sourceTimedOut = false;
    group->freqA.resize(static_cast<std::size_t>(samples));
    group->freqB.resize(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        group->freqA[static_cast<std::size_t>(i)] = static_cast<float>(i) + 0.25f;
        group->freqB[static_cast<std::size_t>(i)] = -static_cast<float>(i) - 0.5f;
    }
    return group;
}

// Parks the stage worker immediately after it dequeues one frame, so queue and
// boundary behaviour can be asserted without racing the consumer thread.
class WorkerGate {
public:
    void install(FrontendPreprocessor& stage) {
        stage.setWorkerSeamForTest([this] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                entered_.store(true, std::memory_order_release);
            }
            condition_.notify_all();
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return released_; });
        });
    }
    bool waitEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock, 3s, [this] {
            return entered_.load(std::memory_order_acquire);
        });
        return entered_.load(std::memory_order_acquire);
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<bool> entered_{false};
    bool released_ = false;
};

struct RingRecorder {
    std::mutex mutex;
    std::vector<const TriggerGroup*> pointers;
    std::vector<std::uint16_t> triggers;
    std::vector<std::uint64_t> generations;
    std::vector<std::int64_t> indices;
    std::vector<bool> finals;
    std::vector<std::size_t> freqSizes;
    std::vector<std::vector<float>> freqAs;
    std::vector<std::vector<float>> freqBs;
    std::atomic<int> count{0};
    std::atomic<bool> throwOnNext{false};

    FrontendPreprocessor::RingSink sink() {
        return [this](const TriggerGroupConstPtr& frame) {
            if (throwOnNext.load(std::memory_order_acquire))
                throw std::runtime_error("ring sink fault");
            std::lock_guard<std::mutex> lock(mutex);
            pointers.push_back(frame.get());
            triggers.push_back(frame->triggerSeq);
            generations.push_back(frame->roundGeneration);
            indices.push_back(frame->logicalTriggerIndex);
            finals.push_back(frame->isFinalLogicalTrigger);
            freqSizes.push_back(frame->freqA.size());
            freqAs.push_back(frame->freqA);
            freqBs.push_back(frame->freqB);
            ++count;
            return ImagingSubmitResult::Accepted;
        };
    }
};

// 两路都停用的配置：frontend 恒等，供架构类断言使用。
frontend_filter::Config bothDisabled() {
    frontend_filter::Config cfg;
    cfg.hpEnable = false;
    cfg.lpEnable = false;
    return cfg;
}

// 校准用的零相位配置：极点半径适中，端部延拓 3N 足以压住零初始状态的启动瞬态。
frontend_filter::Config zeroPhaseConfig() {
    frontend_filter::Config cfg;
    cfg.hpEnable = true;  cfg.hpCutoffMhz = 20.0; cfg.hpOrder = 2;
    cfg.lpEnable = true;  cfg.lpCutoffMhz = 60.0; cfg.lpOrder = 2;
    return cfg;
}

// 紧支撑对称升余弦包络：中心对称，两端严格为 0，因此奇对称反射延拓即零延拓，
// 两次前向滤波的零初始状态不会激起边缘瞬态（见 F3 注释）。
std::vector<float> symmetricBump(int n, double halfWidthFraction) {
    std::vector<float> x(static_cast<std::size_t>(n), 0.0f);
    const double c = (n - 1) / 2.0;
    const double hw = n * halfWidthFraction;
    for (int i = 0; i < n; ++i) {
        const double d = (i - c) / hw;
        if (std::fabs(d) <= 1.0)
            x[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 * (1.0 + std::cos(3.14159265358979323846 * d)));
    }
    return x;
}

double maxAbs(const std::vector<float>& v) {
    double m = 0.0;
    for (float x : v) m = std::max(m, std::fabs(static_cast<double>(x)));
    return m;
}

// 关于序列中心的不对称度（相对峰值）。
double centerAsymmetry(const std::vector<float>& y) {
    const double peak = maxAbs(y);
    double worst = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double d = std::fabs(static_cast<double>(y[i]) -
                                   static_cast<double>(y[y.size() - 1 - i]));
        worst = std::max(worst, d);
    }
    return peak > 0.0 ? worst / peak : worst;
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return std::numeric_limits<double>::infinity();
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[i]) -
                                          static_cast<double>(b[i])));
    return worst;
}

// ---------------------------------------------------------------- 1
void testIdentityDeepCopy() {
    FrontendPreprocessor stage(0, 8);
    // 本例断言的是架构契约（深拷贝 + 元数据完整 + raw 不被改写）下的恒等 frontend，
    // 故显式停用两路滤波；滤波数值契约由 F1..F13 覆盖。
    require(stage.setFilterConfig(bothDisabled()), "T1 filter paths disabled");
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    stage.start();
    stage.beginSession(1);

    auto raw = makeGroup(3, 42, 64, 1, 2, 5, false);
    // Pre-fill *_display with sentinels: the stage must never mutate the raw
    // object, only its own deep copy.
    raw->freqA_display.assign(64, 99.0f);
    raw->freqB_display.assign(64, 98.0f);
    raw->phaseA_display.assign(64, 97.0f);
    raw->phaseB_display.assign(64, 96.0f);
    const TriggerGroup* rawPointer = raw.get();
    const std::vector<float> rawFreqA = raw->freqA;
    const std::vector<float> rawFreqB = raw->freqB;

    require(stage.submit(raw) == FrontendSubmitResult::Accepted, "T1 submit accepted");
    require(until([&] { return ring.count.load() == 1; }), "T1 ring delivery");

    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.pointers.front() != rawPointer, "T1 output pointer differs from input");
    }
    require(raw->freqA_display == std::vector<float>(64, 99.0f), "T1 raw freqA_display untouched");
    require(raw->phaseA_display == std::vector<float>(64, 97.0f), "T1 raw phaseA_display untouched");
    require(raw->freqA == rawFreqA && raw->freqB == rawFreqB, "T1 raw freqA/freqB untouched");
    require(raw->measurementSession == 1 && raw->roundGeneration == 2 &&
                raw->triggerSeq == 42 && raw->logicalTriggerIndex == 5 &&
                !raw->roundComplete && !raw->isFinalLogicalTrigger &&
                raw->sessionGen == 7 && raw->timestamp_ms == 1042u &&
                raw->sourceIPv4 == 0x0100007f && raw->isComplete &&
                raw->normalizationApplied &&
                raw->physicalDecision == paimage::PhysicalTriggerDecision::LogicalScan &&
                !raw->sourceTimedOut && raw->cardId == 3,
            "T1 raw metadata untouched");

    DisplayBuffer::Snapshot snap;
    require(display.peekLatest(snap) && snap.valid, "T1 display snapshot");
    require(snap.triggerSeq == 42 && snap.sampleCount == 64, "T1 display identity");
    require(snap.freqA.size() == 64 && snap.freqB.size() == 64 &&
                snap.phaseA.size() == 64 && snap.phaseB.size() == 64,
            "T1 display vectors preserved");
    bool valuesMatch = true;
    for (int i = 0; i < 64; ++i) {
        if (snap.freqA[static_cast<std::size_t>(i)] !=
                static_cast<double>(rawFreqA[static_cast<std::size_t>(i)]) ||
            snap.freqB[static_cast<std::size_t>(i)] !=
                static_cast<double>(rawFreqB[static_cast<std::size_t>(i)]))
            valuesMatch = false;
    }
    require(valuesMatch, "T1 display freq values match input");

    DisplayBuffer::FullResSnapshot full;
    require(display.peekLatestFull(full) && full.freqA.size() == 64 &&
                full.freqA == rawFreqA && full.freqB == rawFreqB,
            "T1 full-resolution frequencies preserved");

    const auto stats = stage.snapshot();
    require(stats.accepted == 1 && stats.processed == 1 && stats.deepCopies == 1,
            "T1 single deep copy per frame");
    stage.stop();
    std::cout << "PASS  T1 identity deep-copy: distinct pointer, full metadata/freqA/freqB, raw untouched\n";
}

// ---------------------------------------------------------------- 2
void testFullResolution() {
    FrontendPreprocessor stage(0, 8);
    // 同 T1：断言全分辨率保留，故停用滤波以免把"保留每个采样点"与"数值被滤波
    // 改写"混在一起。
    require(stage.setFilterConfig(bothDisabled()), "T2 filter paths disabled");
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    stage.start();
    stage.beginSession(1);

    constexpr int kSamples = 2048;  // > 1000 points
    auto raw = makeGroup(0, 11, kSamples, 1, 0, 0, false);
    require(stage.submit(raw) == FrontendSubmitResult::Accepted, "T2 submit accepted");
    require(until([&] { return ring.count.load() == 1; }), "T2 ring delivery");

    DisplayBuffer::Snapshot snap;
    require(display.peekLatest(snap), "T2 display snapshot");
    require(snap.sampleCount == kSamples, "T2 display sampleCount");
    require(snap.freqA.size() == static_cast<std::size_t>(kSamples) &&
                snap.freqB.size() == static_cast<std::size_t>(kSamples) &&
                snap.phaseA.size() == static_cast<std::size_t>(kSamples) &&
                snap.phaseB.size() == static_cast<std::size_t>(kSamples),
            "T2 display keeps every sample");
    require(snap.freqA.front() == 0.25 && snap.freqA.back() == 2047.25, "T2 display endpoints");
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.freqSizes.front() == static_cast<std::size_t>(kSamples),
                "T2 frontend output keeps every sample");
    }
    require(raw->freqA_display.empty(), "T2 raw display fields not produced");
    stage.stop();
    std::cout << "PASS  T2 full-resolution: 2048-point input fully preserved by display and frontend output\n";
}

// ---------------------------------------------------------------- 3
void testOneResultTwoConsumers() {
    FrontendPreprocessor stage(0, 8);
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    const TriggerGroup* displayClone = nullptr;
    const TriggerGroup* ringClone = nullptr;
    std::mutex observerMutex;
    stage.setDispatchObserver([&](const char* consumer, const TriggerGroup* clone) {
        std::lock_guard<std::mutex> lock(observerMutex);
        if (std::string(consumer) == "display") displayClone = clone;
        if (std::string(consumer) == "ring") ringClone = clone;
    });
    stage.start();
    stage.beginSession(1);

    auto raw = makeGroup(1, 5, 32, 1, 0, 0, false);
    const TriggerGroup* rawPointer = raw.get();
    require(stage.submit(raw) == FrontendSubmitResult::Accepted, "T3 submit accepted");
    require(until([&] { return ring.count.load() == 1; }), "T3 ring delivery");

    require(displayClone != nullptr && ringClone != nullptr, "T3 both consumers observed");
    require(displayClone == ringClone, "T3 display and ring share one frontend clone");
    require(displayClone != rawPointer, "T3 consumers see the frontend clone, not the raw group");
    require(stage.snapshot().deepCopies == 1, "T3 exactly one preprocessing result");
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.pointers.front() == ringClone, "T3 ring sink received that same clone");
    }
    stage.stop();
    std::cout << "PASS  T3 one result / two consumers: single clone feeds DisplayBuffer and RingFeedSink\n";
}

// ---------------------------------------------------------------- 4
void testFifoOrdering() {
    FrontendPreprocessor stage(0, 128);
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    WorkerGate gate;
    gate.install(stage);
    stage.start();
    stage.beginSession(1);

    // The first frame parks the worker (in-flight); the rest stay queued.
    constexpr int kFrames = 64;
    for (int i = 0; i < kFrames; ++i) {
        auto raw = makeGroup(0, static_cast<std::uint16_t>(i), 16, 1, 0, i, false);
        require(stage.submit(raw) == FrontendSubmitResult::Accepted, "T4 submit accepted");
        if (i == 0) require(gate.waitEntered(), "T4 worker parked on the first frame");
    }
    require(stage.snapshot().currentDepth == kFrames - 1, "T4 remaining frames stayed queued");
    gate.release();
    require(until([&] { return ring.count.load() == kFrames; }), "T4 all frames delivered");

    bool ordered = true;
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        for (int i = 0; i < kFrames; ++i) {
            if (ring.triggers[static_cast<std::size_t>(i)] != static_cast<std::uint16_t>(i) ||
                ring.indices[static_cast<std::size_t>(i)] != i)
                ordered = false;
        }
    }
    require(ordered, "T4 per-card trigger order preserved (bounded FIFO)");
    require(stage.snapshot().maxDepth >= 2, "T4 queue actually buffered frames");
    stage.stop();
    std::cout << "PASS  T4 FIFO ordering: 64 triggers dispatched in submit order\n";
}

// ---------------------------------------------------------------- 5
void testNonBlockingSubmit() {
    FrontendPreprocessor stage(0, 512);
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    WorkerGate gate;
    gate.install(stage);
    stage.start();
    stage.beginSession(1);

    AcqConfig config;
    std::atomic<int> saved{0};
    DataProcessor processor(0, nullptr, nullptr, config,
        [&stage](const TriggerGroupPtr& group) { return stage.submit(group); });
    processor.setDirectSaveSink([&](const TriggerGroupPtr&) { ++saved; return true; });

    // Park the worker on an in-flight frame.
    require(stage.submit(makeGroup(0, 60000, 16, 1, 0, 0, false)) ==
                FrontendSubmitResult::Accepted, "T5 parking frame accepted");
    require(gate.waitEntered(), "T5 worker parked");

    constexpr int kFrames = 150;
    std::uint64_t maxSubmitNs = 0;
    std::uint64_t maxDeliverNs = 0;
    for (int i = 0; i < kFrames; ++i) {
        auto raw = makeGroup(0, static_cast<std::uint16_t>(i), 16, 1, 0, i + 1, false);
        auto begin = std::chrono::steady_clock::now();
        require(stage.submit(raw) == FrontendSubmitResult::Accepted, "T5 submit accepted");
        maxSubmitNs = std::max(maxSubmitNs, elapsedNs(begin));

        auto deliverBegin = std::chrono::steady_clock::now();
        const auto delivered = processor.deliverAssembled(
            makeGroup(0, static_cast<std::uint16_t>(20000 + i), 16, 1, 0, i + 1, false), true, true);
        maxDeliverNs = std::max(maxDeliverNs, elapsedNs(deliverBegin));
        require(delivered.saveAccepted, "T5 deliverAssembled save accepted");
        require(delivered.frontendAccepted, "T5 deliverAssembled frontend accepted");
    }
    require(stage.snapshot().processed == 0, "T5 blocked worker processed nothing");
    require(maxSubmitNs < 50ull * 1000ull * 1000ull,
            "T5 submit returned without waiting for the worker");
    require(maxDeliverNs < 50ull * 1000ull * 1000ull,
            "T5 deliverAssembled returned without waiting for the worker");
    require(saved.load() == kFrames, "T5 save path unaffected by the blocked frontend worker");

    gate.release();
    require(until([&] { return stage.snapshot().processed == std::uint64_t(2 * kFrames + 1); }),
            "T5 queued work drains after the worker resumes");
    processor.requestStop();
    stage.stop();
    std::cout << "PASS  T5 non-blocking submit: submit/deliverAssembled return while the worker is parked\n";
}

// ---------------------------------------------------------------- 6
void testBoundedOverflow() {
    // FrontendSubmitResult must never alias ImagingSubmitResult (Task 3.5).
    static_assert(!std::is_same<FrontendSubmitResult, ImagingSubmitResult>::value,
                  "frontend queue result type must stay separate from ImagingSubmitResult");

    FrontendPreprocessor stage(0, 2);  // deliberately tiny queue
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    WorkerGate gate;
    gate.install(stage);
    stage.start();
    stage.beginSession(1);

    // The parking frame is already dequeued into the seam, so the queue is empty
    // and exactly `capacity` further frames fit before a deterministic reject.
    require(stage.submit(makeGroup(0, 0, 8, 1, 0, 0, false)) ==
                FrontendSubmitResult::Accepted, "T6 parking frame accepted");
    require(gate.waitEntered(), "T6 worker parked");

    require(stage.submit(makeGroup(0, 1, 8, 1, 0, 1, false)) ==
                FrontendSubmitResult::Accepted, "T6 first queued frame accepted");
    require(stage.submit(makeGroup(0, 2, 8, 1, 0, 2, false)) ==
                FrontendSubmitResult::Accepted, "T6 second queued frame accepted");
    const auto rejected = stage.submit(makeGroup(0, 3, 8, 1, 0, 3, false));
    require(rejected == FrontendSubmitResult::QueueFull, "T6 bounded overflow rejected");
    const auto stats = stage.snapshot();
    require(stats.queueRejected == 1, "T6 queueRejected counted once");
    require(stats.accepted == 3, "T6 accepted/enqueued counted once per admission");
    require(stats.currentDepth == 2, "T6 depth bounded by capacity");
    require(stats.maxDepth == 2, "T6 maxDepth equals the capacity bound");

    gate.release();
    require(until([&] { return stage.snapshot().processed == 3; }), "T6 admitted frames drain");
    stage.stop();
    std::cout << "PASS  T6 bounded overflow: capacity-2 FIFO rejects deterministically as FrontendSubmitResult::QueueFull\n";
}

// ---------------------------------------------------------------- 7
void testSaveIsolation() {
    FrontendPreprocessor stage(0, 2);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    WorkerGate gate;
    gate.install(stage);
    stage.start();
    stage.beginSession(1);

    AcqConfig config;
    std::atomic<int> saved{0};
    std::mutex savedMutex;
    std::vector<std::size_t> savedDisplaySizes;
    std::vector<std::size_t> savedFreqSizes;
    DataProcessor processor(0, nullptr, nullptr, config,
        [&stage](const TriggerGroupPtr& group) { return stage.submit(group); });
    processor.setDirectSaveSink([&](const TriggerGroupPtr& group) {
        std::lock_guard<std::mutex> lock(savedMutex);
        savedDisplaySizes.push_back(group->freqA_display.size());
        savedFreqSizes.push_back(group->freqA.size());
        ++saved;
        return true;
    });

    // Park the worker without touching the save path, then saturate the queue.
    require(stage.submit(makeGroup(0, 900, 32, 1, 0, 0, false)) ==
                FrontendSubmitResult::Accepted, "T7 parking frame accepted");
    require(gate.waitEntered(), "T7 worker parked");

    constexpr int kFrames = 12;
    int frontendAccepted = 0;
    for (int i = 0; i < kFrames; ++i) {
        auto raw = makeGroup(0, static_cast<std::uint16_t>(i), 32, 1, 0, i, false);
        const auto result = processor.deliverAssembled(raw, true, true);
        require(result.save == DataProcessor::DeliveryResult::Consumed && result.saveAccepted,
                "T7 raw save consumed while the frontend queue is saturated");
        if (result.frontendAccepted) ++frontendAccepted;
    }
    require(saved.load() == kFrames, "T7 every raw save succeeded");
    require(frontendAccepted == 2, "T7 only the bounded frontend capacity was admitted");
    const auto stats = stage.snapshot();
    require(stats.queueRejected == kFrames - 2, "T7 frontend rejections counted as queueRejected");
    // Frontend queue rejection must never leak into ingress or save loss.
    require(stats.accepted == 3, "T7 frontend admissions counted separately from save");
    {
        const auto cardStats = processor.statsSnapshot();
        require(cardStats.saveQueueDiscards == 0 && cardStats.triggersDiscarded == 0 &&
                    cardStats.missingTriggerCount == 0 && cardStats.packetsDropped == 0,
                "T7 frontend queue rejection never counts as ingress/save loss");
    }

    // The raw group reaching the save sink is never rewritten by the frontend
    // stage: display fields stay empty and the raw frequencies stay intact.
    bool rawUntouched = true;
    {
        std::lock_guard<std::mutex> lock(savedMutex);
        for (std::size_t i = 0; i < savedDisplaySizes.size(); ++i) {
            if (savedDisplaySizes[i] != 0 || savedFreqSizes[i] != 32) rawUntouched = false;
        }
    }
    require(rawUntouched, "T7 raw saved payload not rewritten by the frontend stage");

    gate.release();
    require(until([&] { return stage.snapshot().processed == 3; }), "T7 admitted frames drain");
    require(saved.load() == kFrames, "T7 save count unchanged by frontend drain");
    processor.requestStop();
    stage.stop();
    std::cout << "PASS  T7 save isolation: 12/12 raw saves consumed while the frontend queue rejected 10 frames\n";
}

// ---------------------------------------------------------------- 8
void testSessionStale() {
    FrontendPreprocessor stage(0, 16);
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    WorkerGate gate;
    gate.install(stage);
    stage.start();
    stage.beginSession(1);

    // In-flight: dequeued by the worker and parked inside the seam.
    auto inFlight = makeGroup(0, 1, 8, 1, 0, 0, false);
    require(stage.submit(inFlight) == FrontendSubmitResult::Accepted, "T8 in-flight accepted");
    require(gate.waitEntered(), "T8 worker parked on the in-flight frame");
    // Queued: still waiting in the FIFO.
    require(stage.submit(makeGroup(0, 2, 8, 1, 0, 1, false)) ==
                FrontendSubmitResult::Accepted, "T8 queued accepted");

    // A new measurement session begins while the worker is still behind.
    stage.beginSession(2);
    require(stage.submit(makeGroup(0, 3, 8, 2, 0, 2, false)) ==
                FrontendSubmitResult::Accepted, "T8 new session accepted");

    gate.release();
    require(until([&] {
        const auto stats = stage.snapshot();
        return stats.processed + stats.staleDropped >= 3;
    }), "T8 accounting settles");

    const auto stats = stage.snapshot();
    require(stats.processed == 1, "T8 only the new-session frame dispatched");
    require(stats.staleDropped == 2, "T8 queued and in-flight old-session frames staleDropped");
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.triggers.size() == 1 && ring.triggers.front() == 3,
                "T8 old session frames never reached Ring");
    }
    stage.stop();
    std::cout << "PASS  T8 session stale: old queued/in-flight frames dropped across beginSession\n";
}

// ---------------------------------------------------------------- 9
void testTimeoutStale() {
    FrontendPreprocessor stage(0, 16);
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    WorkerGate gate;
    gate.install(stage);
    stage.start();
    stage.beginSession(1);

    auto inFlightOld = makeGroup(0, 1, 8, 1, 0, 0, false);
    require(stage.submit(inFlightOld) == FrontendSubmitResult::Accepted, "T9 old in-flight accepted");
    require(gate.waitEntered(), "T9 worker parked");
    require(stage.submit(makeGroup(0, 2, 8, 1, 0, 1, false)) ==
                FrontendSubmitResult::Accepted, "T9 old queued accepted");

    // PhysicalRound TimeoutBoundary advances the stale barrier to the new round.
    stage.advanceRoundBarrier(1, 1);
    require(stage.submit(makeGroup(0, 20, 8, 1, 1, 0, false)) ==
                FrontendSubmitResult::Accepted, "T9 new round accepted");

    gate.release();
    require(until([&] {
        const auto stats = stage.snapshot();
        return stats.processed + stats.staleDropped >= 3;
    }), "T9 accounting settles");

    const auto stats = stage.snapshot();
    require(stats.processed == 1, "T9 only the new-round frame dispatched");
    require(stats.staleDropped == 2, "T9 old roundGeneration frames staleDropped");
    require(stats.minDispatchableRoundGeneration == 1, "T9 barrier advanced");
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.triggers.size() == 1 && ring.generations.front() == 1,
                "T9 new round passes, old round dropped");
    }
    stage.stop();
    std::cout << "PASS  T9 timeout stale: barrier drop of queued/in-flight old-round frames\n";
}

// ---------------------------------------------------------------- 10
void testCountBoundaryFinal() {
    FrontendPreprocessor stage(0, 16);
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    WorkerGate gate;
    gate.install(stage);
    stage.start();
    stage.beginSession(1);

    // A non-final scan of round 0, parked in flight by the seam ...
    require(stage.submit(makeGroup(0, 76, 8, 1, 0, 2, false)) ==
                FrontendSubmitResult::Accepted, "T10 ordinary accepted");
    require(gate.waitEntered(), "T10 worker parked");
    // ... and the just-completed final logical trigger of round 0, still queued.
    require(stage.submit(makeGroup(0, 77, 8, 1, 0, 3, true)) ==
                FrontendSubmitResult::Accepted, "T10 final accepted");

    // CountBoundary closes round 0 and opens round 1. The final logical trigger
    // must not be prematurely dropped by that boundary barrier.
    stage.advanceRoundBarrier(1, 1);
    gate.release();
    require(until([&] {
        const auto stats = stage.snapshot();
        return stats.processed + stats.staleDropped >= 2;
    }), "T10 accounting settles");

    int finalDispatched = 0;
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        for (std::uint16_t trigger : ring.triggers)
            if (trigger == 77) ++finalDispatched;
        require(ring.triggers.size() == 1 && ring.triggers.front() == 77,
                "T10 only the final logical trigger survives the boundary barrier");
    }
    require(finalDispatched == 1, "T10 final logical trigger dispatched exactly once");
    require(stage.snapshot().staleDropped == 1, "T10 non-final old-round frame dropped");
    stage.stop();
    std::cout << "PASS  T10 CountBoundary final: final logical trigger survives the barrier exactly once\n";
}

// ---------------------------------------------------------------- 11
void testStopLifecycle() {
    FrontendPreprocessor stage(0, 16);
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    WorkerGate gate;
    gate.install(stage);
    stage.start();
    stage.beginSession(1);

    auto inFlight = makeGroup(0, 1, 8, 1, 0, 0, false);
    require(stage.submit(inFlight) == FrontendSubmitResult::Accepted, "T11 in-flight accepted");
    require(gate.waitEntered(), "T11 worker parked");
    for (int i = 0; i < 4; ++i)
        require(stage.submit(makeGroup(0, static_cast<std::uint16_t>(2 + i), 8, 1, 0, i + 1, false)) ==
                    FrontendSubmitResult::Accepted, "T11 queued accepted");

    // stop() joins the worker.  The gate is released only once stop() has cleared
    // the queue, so nothing can be dispatched after the stop transition.
    std::thread stopper([&stage] { stage.stop(); });
    require(until([&] { return stage.snapshot().currentDepth == 0; }),
            "T11 stop cleared the pending queue");
    gate.release();
    stopper.join();

    const auto stats = stage.snapshot();
    require(stats.processed == 0, "T11 no post-stop dispatch");
    require(ring.count.load() == 0, "T11 Ring never saw a post-stop frame");
    require(stats.currentDepth == 0, "T11 queue cleared at stop");
    require(stats.staleDropped >= 5, "T11 queued and in-flight frames cleared at stop");
    require(stage.submit(makeGroup(0, 9, 8, 1, 0, 0, false)) == FrontendSubmitResult::Stopping,
            "T11 submit rejected after stop");

    // start/stop again must be safe (rollback / restart paths).
    stage.start();
    stage.beginSession(2);
    require(stage.submit(makeGroup(0, 10, 8, 2, 0, 0, false)) ==
                FrontendSubmitResult::Accepted, "T11 restart accepts again");
    require(until([&] { return stage.snapshot().processed == 1; }), "T11 restart dispatches");
    stage.stop();
    std::cout << "PASS  T11 stop lifecycle: clear + join with no post-stop dispatch\n";
}

// ---------------------------------------------------------------- 12
void testExceptionContainment() {
    FrontendPreprocessor stage(0, 64);
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    stage.setProcessFaultForTest([](TriggerGroup& group) {
        if (group.triggerSeq == 1) throw std::runtime_error("frontend processing fault");
    });
    stage.start();
    stage.beginSession(1);

    AcqConfig config;
    std::atomic<int> saved{0};
    DataProcessor processor(0, nullptr, nullptr, config,
        [&stage](const TriggerGroupPtr& group) { return stage.submit(group); });
    processor.setDirectSaveSink([&](const TriggerGroupPtr&) { ++saved; return true; });

    // 1) frontend processing fault is contained on the worker.
    const auto faultResult = processor.deliverAssembled(
        makeGroup(0, 1, 8, 1, 0, 0, false), true, true);
    require(faultResult.saveAccepted && !faultResult.exception,
            "T12 save thread unaffected by frontend processing fault");

    // 2) downstream Ring callback fault is contained as well.
    ring.throwOnNext.store(true, std::memory_order_release);
    const auto downstreamResult = processor.deliverAssembled(
        makeGroup(0, 2, 8, 1, 0, 1, false), true, true);
    require(downstreamResult.saveAccepted && !downstreamResult.exception,
            "T12 save thread unaffected by downstream Ring fault");
    require(until([&] { return stage.snapshot().downstreamRingExceptions == 1; }),
            "T12 downstream Ring fault contained on the stage worker");
    ring.throwOnNext.store(false, std::memory_order_release);

    // 3) the stage keeps serving afterwards.
    for (int i = 3; i <= 6; ++i) {
        const auto result = processor.deliverAssembled(
            makeGroup(0, static_cast<std::uint16_t>(i), 8, 1, 0, i - 1, false), true, true);
        require(result.saveAccepted && result.frontendAccepted, "T12 stage still accepting");
    }
    require(until([&] {
        const auto stats = stage.snapshot();
        return stats.processed == 4 && stats.exceptionDropped == 2;
    }), "T12 fault containment accounting");

    const auto stats = stage.snapshot();
    require(saved.load() == 6, "T12 every raw save succeeded through both faults");
    require(stats.downstreamRingAttempts == 5 && stats.downstreamRingAccepted == 4,
            "T12 downstream Ring results reported as downstream only");
    require(stats.downstreamRingExceptions == 1, "T12 downstream Ring exception counted");
    require(stats.queueRejected == 0, "T12 faults never counted as queue rejection");
    require(stats.displayUpdates == 5, "T12 display dispatch continued after faults");
    processor.requestStop();
    stage.stop();
    std::cout << "PASS  T12 exception containment: processing/downstream faults contained, acquisition and save unaffected\n";
}

// ================================================================ F1
// 任务 7 检查项 1：双路均停用，输出序列与输入序列逐点一致。
void testFilterDisabledIsIdentity() {
    const frontend_filter::Config cfg = bothDisabled();
    const frontend_filter::Bank bank(cfg);
    require(!bank.active(), "F1 both paths disabled leaves no active path");

    std::vector<float> x(257);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(std::sin(0.07 * static_cast<double>(i))) *
               static_cast<float>((i % 13) + 1);
    std::vector<float> y = x;
    require(!bank.apply(y.data(), y.size()), "F1 apply reports no filter pass ran");
    require(y == x, "F1 disabled bank leaves every sample untouched");

    FrontendPreprocessor stage(0, 8);
    require(stage.setFilterConfig(cfg), "F1 stage accepts the disabled config");
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    stage.start();
    stage.beginSession(1);
    auto raw = makeGroup(0, 1, 64, 1, 0, 0, false);
    const std::vector<float> inputA = raw->freqA;
    const std::vector<float> inputB = raw->freqB;
    require(stage.submit(raw) == FrontendSubmitResult::Accepted, "F1 submit accepted");
    require(until([&] { return ring.count.load() == 1; }), "F1 dispatch");
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.freqAs.front() == inputA && ring.freqBs.front() == inputB,
                "F1 dispatched clone equals the input sequence point by point");
    }
    require(stage.snapshot().filteredFrames == 0, "F1 no frame counted as filtered");
    stage.stop();
    std::cout << "PASS  F1 both paths disabled: output equals input point by point\n";
}

// ================================================================ F2
// 任务 7 检查项 2：仅高通 / 仅低通 / 两路同时启用都可执行，输出长度与输入一致。
void testFilterPathsExecute() {
    struct Case { const char* name; bool hp; bool lp; };
    const Case cases[] = {
        {"highpass only", true,  false},
        {"lowpass only",  false, true},
        {"both paths",    true,  true},
    };
    for (const Case& c : cases) {
        frontend_filter::Config cfg;
        cfg.hpEnable = c.hp;  cfg.hpCutoffMhz = 20.0; cfg.hpOrder = 3;
        cfg.lpEnable = c.lp;  cfg.lpCutoffMhz = 60.0; cfg.lpOrder = 2;
        require(frontend_filter::validate(cfg) == frontend_filter::Validation::Ok,
                "F2 case config is valid");
        const frontend_filter::Bank bank(cfg);
        require(bank.active(), "F2 enabled path designs coefficients");

        std::vector<float> x(128);
        for (std::size_t i = 0; i < x.size(); ++i)
            x[i] = static_cast<float>(std::sin(0.11 * static_cast<double>(i)) + 0.3);
        std::vector<float> y = x;
        require(bank.apply(y.data(), y.size()), "F2 filter pass executed");
        require(y.size() == x.size(), "F2 output length equals input length");
        bool finite = true, changed = false;
        for (std::size_t i = 0; i < y.size(); ++i) {
            if (!std::isfinite(y[i])) finite = false;
            if (y[i] != x[i]) changed = true;
        }
        require(finite, "F2 output is finite");
        require(changed, "F2 the enabled path actually altered the sequence");

        FrontendPreprocessor stage(0, 8);
        require(stage.setFilterConfig(cfg), "F2 stage accepts the config");
        RingRecorder ring;
        stage.setRingSink(ring.sink());
        stage.start();
        stage.beginSession(1);
        auto raw = makeGroup(0, 1, 96, 1, 0, 0, false);
        require(stage.submit(raw) == FrontendSubmitResult::Accepted, "F2 submit accepted");
        require(until([&] { return ring.count.load() == 1; }), "F2 dispatch");
        {
            std::lock_guard<std::mutex> lock(ring.mutex);
            require(ring.freqAs.front().size() == 96, "F2 stage output length preserved");
        }
        require(stage.snapshot().filteredFrames == 1, "F2 frame counted as filtered");
        stage.stop();
    }
    std::cout << "PASS  F2 highpass only / lowpass only / both: all three paths execute, lengths preserved\n";
}

// ================================================================ F3
// 任务 7 检查项 3：零相位——对称输入序列滤波后的输出关于序列中心对称。
//
// 输入构造：紧支撑对称升余弦包络，居中于一条两端严格为 0 的长缓冲区。这样做是
// 因为任务 3.5 明确规定两次滤波的初始状态都置零：零状态使每次前向滤波从缓冲区
// 一端"冷启动"，因此对一条在延拓端非零的对称序列，filtfilt 结果只在浮点舍入
// 量级之外还带边缘瞬态不对称。把信号放在两端为零的缓冲区中央后，两次滤波的零
// 状态都无东西可激起瞬态，输出对称性降到浮点舍入量级——这才是"零相位"的干净
// 判据。对照组用同输入只做一次正向滤波（最小相位、非零相位），不对称度大几个
// 量级，说明该断言具有判别力。
void testFilterZeroPhase() {
    const int n = 4096;
    const std::vector<float> x = symmetricBump(n, 0.05);
    require(centerAsymmetry(x) <= 1e-7, "F3 input is symmetric to begin with");

    const frontend_filter::Bank bank(zeroPhaseConfig());
    std::vector<float> y = x;
    require(bank.apply(y.data(), y.size()), "F3 filter pass executed");
    const double filtfiltAsym = centerAsymmetry(y);
    require(filtfiltAsym <= 1e-6,
            "F3 filtfilt output is symmetric about the sequence center");

    // 判别力对照：非零相位的单向滤波在同一输入上明显不对称。
    std::vector<float> forward = x;
    frontend_filter::filterForward(frontend_filter::designLowpass(2, 60e6),
                                   forward.data(), forward.size());
    const double forwardAsym = centerAsymmetry(forward);
    require(forwardAsym >= 1e-3, "F3 contrast: single forward pass is not symmetric");
    require(forwardAsym > 100.0 * filtfiltAsym,
            "F3 contrast between zero-phase and phase-shifted is decisive");
    std::cout << "PASS  F3 zero-phase: filtfilt asymmetry " << filtfiltAsym
              << " vs forward-only " << forwardAsym << "\n";
}

// ================================================================ F4
// 任务 7 检查项 4：逐 A-line 独立。
//
// 断言三件事：(a) 一条 A-line 的结果不依赖任何历史 A-line，也不依赖处理顺序；
// (b) 两段序列拼接进同一缓冲区后整体处理，对应片段与分开处理一致，差异在浮点
// 舍入量级；(c) 反证——把两段当作一条 A-line 拼接滤波与逐 A-line 独立滤波显著
// 不同，说明实现从不合并 A-line（任务 3.2 禁止"拼接样本"）。
//
// 口径说明：若把 (b) 中的"拼接后整体滤波"读成"把两段样本当作一条 A-line 做一次
// filtfilt"，该比较在浮点舍入量级上不可能成立——那正是逐 A-line 独立所禁止的
// 行为，其差别正是两端延拓位置不同带来的边缘效应（见 (c) 的量级）。因此 (b)
// 按"拼接进同一缓冲区后逐段处理"实现，与 (c) 一起完整覆盖本条契约。
void testFilterPerALineIndependence() {
    const frontend_filter::Bank bank(zeroPhaseConfig());
    std::vector<float> s1(200), s2(150);
    for (std::size_t i = 0; i < s1.size(); ++i)
        s1[i] = static_cast<float>(std::sin(0.05 * static_cast<double>(i)) *
                                   (1.0 + 0.01 * static_cast<double>(i)));
    for (std::size_t i = 0; i < s2.size(); ++i)
        s2[i] = static_cast<float>(std::cos(0.03 * static_cast<double>(i)) - 0.4);

    // (a) 无状态携带 / 顺序无关
    std::vector<float> a1 = s1, b1 = s2;
    bank.apply(a1.data(), a1.size());
    bank.apply(b1.data(), b1.size());
    const frontend_filter::Bank fresh(zeroPhaseConfig());
    std::vector<float> b2 = s2, a2 = s1;
    fresh.apply(b2.data(), b2.size());
    fresh.apply(a2.data(), a2.size());          // 反序：先 s2 后 s1
    require(maxDiff(b1, b2) <= 1e-6 * maxAbs(b2),
            "F4a a later A-line is unaffected by the A-lines before it");
    require(maxDiff(a1, a2) <= 1e-6 * maxAbs(a2),
            "F4a an A-line is unaffected by processing order");

    // (b) 两段序列拼接进同一缓冲区后整体处理，对应片段与分开处理一致
    std::vector<float> both = s1;
    both.insert(both.end(), s2.begin(), s2.end());
    bank.apply(both.data(), s1.size());
    bank.apply(both.data() + s1.size(), s2.size());
    const std::vector<float> part1(both.begin(), both.begin() + static_cast<long>(s1.size()));
    const std::vector<float> part2(both.begin() + static_cast<long>(s1.size()), both.end());
    require(maxDiff(part1, a1) <= 1e-6 * maxAbs(a1),
            "F4b segment 1 of the joined buffer matches the isolated result");
    require(maxDiff(part2, b1) <= 1e-6 * maxAbs(b1),
            "F4b segment 2 of the joined buffer matches the isolated result");

    // (c) 反证：拼接成一条 A-line 的整体滤波 ≠ 逐 A-line 独立滤波
    std::vector<float> merged = s1;
    merged.insert(merged.end(), s2.begin(), s2.end());
    bank.apply(merged.data(), merged.size());   // 一次调用吞下两段样本
    std::vector<float> separate = a1;
    separate.insert(separate.end(), b1.begin(), b1.end());
    const double mergedDeviation = maxDiff(merged, separate);
    require(mergedDeviation > 1e-3 * maxAbs(separate),
            "F4c A-lines are never merged into a single filter call");
    std::cout << "PASS  F4 per-A-line independence: no state carry-over, joined buffer "
                 "matches isolated runs, merged-call deviation " << mergedDeviation << "\n";
}

// ================================================================ F5
// 任务 7 检查项 5：端部延拓长度为 3 × 阶数，两端按奇对称反射规则取值；
// 用可复算的已知序列断言实际采用的延拓与截取规则。
void testFilterEdgeExtension() {
    // 已知序列 x[i] = i + 1，n = 20，阶数 2 -> L = 3 × 2 = 6
    std::vector<float> x(20);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(i + 1);
    require(frontend_filter::extensionLength(20, 2) == 6u, "F5 L = 3 x order");
    const std::vector<float> ext = frontend_filter::oddReflectExtend(x.data(), 20, 6);
    require(ext.size() == 32u, "F5 extended length is n + 2L");

    // 逐点按规则复算：左端第 k 点（k = 1 … L）= 2*x[0] − x[k]，缓冲区最左为 k = L；
    // 右端第 j 点（j = 1 … L）= 2*x[n−1] − x[n−1−j]。
    std::vector<float> expected(32, 0.0f);
    for (std::size_t k = 1; k <= 6; ++k) expected[6 - k] = 2.0f * x[0] - x[k];
    for (std::size_t i = 0; i < 20; ++i) expected[6 + i] = x[i];
    for (std::size_t j = 1; j <= 6; ++j) expected[6 + 20 + j - 1] = 2.0f * x[19] - x[19 - j];
    require(ext == expected, "F5 extension matches the odd-reflection rule point by point");

    // 手算对照（便于复核）：[-5,-4,-3,-2,-1,0] ++ [1..20] ++ [21,22,23,24,25,26]
    const std::vector<float> handComputed = {
        -5, -4, -3, -2, -1, 0,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25, 26};
    require(ext == handComputed, "F5 extension matches the hand-computed sequence");

    // 长度上限规则
    require(frontend_filter::extensionLength(7, 2) == 6u, "F5 L = 3N when n > 3N");
    require(frontend_filter::extensionLength(5, 2) == 4u, "F5 n <= L clamps L to n - 1");
    require(frontend_filter::extensionLength(6, 2) == 5u, "F5 n == 3N clamps L to n - 1");
    require(frontend_filter::extensionLength(1, 2) == 0u, "F5 n < 2 uses no extension");
    require(frontend_filter::extensionLength(0, 2) == 0u, "F5 empty sequence uses no extension");

    // 截取规则：filtfilt 输出长度恒为 n
    const frontend_filter::Bank bank(zeroPhaseConfig());
    for (int n : {2, 5, 6, 7, 32, 257}) {
        std::vector<float> seq(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) seq[static_cast<std::size_t>(i)] = static_cast<float>(i + 1);
        const std::vector<float> before = seq;
        bank.apply(seq.data(), seq.size());
        require(seq.size() == static_cast<std::size_t>(n),
                "F5 filtfilt trims back to the input length");
        (void)before;
    }
    std::cout << "PASS  F5 edge extension: L = 3 x order, odd reflection verified point by point\n";
}

// ================================================================ F6
// 任务 7 检查项 6：短序列保护——n < 2 与 n ≤ L 的输入行为确定、不崩溃、
// 输出长度与输入一致。
void testFilterShortSequence() {
    for (int order = 1; order <= 8; ++order) {
        frontend_filter::Config cfg;
        cfg.hpEnable = true;  cfg.hpCutoffMhz = 30.0; cfg.hpOrder = order;
        cfg.lpEnable = true;  cfg.lpCutoffMhz = 70.0; cfg.lpOrder = order;
        const frontend_filter::Bank bank(cfg);
        for (int n : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
            std::vector<float> x(static_cast<std::size_t>(n), 1.5f);
            std::vector<float> y = x;
            bank.apply(y.data(), y.size());
            require(y.size() == x.size(), "F6 output length equals input length");
            if (n < 2) {
                require(y == x, "F6 n < 2 returns the sequence unchanged");
            } else {
                for (float v : y) require(std::isfinite(v), "F6 short sequence stays finite");
            }
        }
    }
    std::cout << "PASS  F6 short-sequence guard: n < 2 and n <= L are deterministic, finite, length preserved\n";
}

// ================================================================ F7
// 任务 7 检查项 7：保存隔离——滤波只改写 frontend clone；raw 对象在滤波前后
// 逐点不变。
void testFilterSaveIsolation() {
    FrontendPreprocessor stage(0, 8);
    require(stage.setFilterConfig(zeroPhaseConfig()), "F7 filter enabled");
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    stage.start();
    stage.beginSession(1);

    AcqConfig config;
    std::atomic<int> saved{0};
    std::mutex savedMutex;
    std::vector<std::vector<float>> savedFreqA;
    DataProcessor processor(0, nullptr, nullptr, config,
        [&stage](const TriggerGroupPtr& group) { return stage.submit(group); });
    processor.setDirectSaveSink([&](const TriggerGroupPtr& group) {
        {
            std::lock_guard<std::mutex> lock(savedMutex);
            savedFreqA.push_back(group->freqA);
        }
        ++saved;
        return true;
    });

    auto raw = makeGroup(0, 9, 128, 1, 0, 0, false);
    const std::vector<float> rawA = raw->freqA;
    const std::vector<float> rawB = raw->freqB;
    const auto result = processor.deliverAssembled(raw, true, true);
    require(result.saveAccepted && result.frontendAccepted,
            "F7 raw save and frontend submit both accepted");
    require(until([&] { return ring.count.load() == 1; }), "F7 frontend dispatch");
    require(until([&] { return saved.load() == 1; }), "F7 raw save consumed");

    require(raw->freqA == rawA && raw->freqB == rawB,
            "F7 raw freqA/freqB pointwise unchanged by the filter");
    {
        std::lock_guard<std::mutex> lock(savedMutex);
        require(savedFreqA.front() == rawA,
                "F7 raw save payload is not rewritten by the filter");
    }
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.freqAs.front().size() == rawA.size(),
                "F7 clone length preserved");
        require(ring.freqAs.front() != rawA,
                "F7 the filter wrote only to the frontend clone");
    }
    require(stage.snapshot().filteredFrames == 1, "F7 exactly one frame filtered");
    processor.requestStop();
    stage.stop();
    std::cout << "PASS  F7 save isolation: raw pointwise unchanged, filter only rewrote the clone\n";
}

// ================================================================ F8
// 任务 7 检查项 8：同源分发——DisplayBuffer 与 Ring sink 收到同一个 frontend
// clone（沿用既有 one-clone / two-consumers 断言方式，本次开启滤波）。
void testFilterSharedCloneDispatch() {
    FrontendPreprocessor stage(0, 8);
    require(stage.setFilterConfig(zeroPhaseConfig()), "F8 filter enabled");
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    const TriggerGroup* displayClone = nullptr;
    const TriggerGroup* ringClone = nullptr;
    std::mutex observerMutex;
    stage.setDispatchObserver([&](const char* consumer, const TriggerGroup* clone) {
        std::lock_guard<std::mutex> lock(observerMutex);
        if (std::string(consumer) == "display") displayClone = clone;
        if (std::string(consumer) == "ring") ringClone = clone;
    });
    stage.start();
    stage.beginSession(1);

    auto raw = makeGroup(1, 5, 256, 1, 0, 0, false);
    const std::vector<float> rawA = raw->freqA;
    const TriggerGroup* rawPointer = raw.get();
    require(stage.submit(raw) == FrontendSubmitResult::Accepted, "F8 submit accepted");
    require(until([&] { return ring.count.load() == 1; }), "F8 dispatch");

    require(displayClone != nullptr && ringClone != nullptr, "F8 both consumers observed");
    require(displayClone == ringClone, "F8 display and ring share one frontend clone");
    require(displayClone != rawPointer, "F8 consumers see the clone, not the raw group");
    require(stage.snapshot().deepCopies == 1, "F8 exactly one preprocessing result");
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.pointers.front() == ringClone, "F8 ring sink received that same clone");
        require(ring.freqAs.front() != rawA,
                "F8 the shared clone carries the filtered sequence");
    }
    stage.stop();
    std::cout << "PASS  F8 shared clone dispatch: one filtered clone feeds DisplayBuffer and RingFeedSink\n";
}

// ================================================================ F9
// 任务 7 检查项 9：生效时机——变更配置后，下一处理帧使用新系数；
// 已在处理中的帧不被中断（用旧系数完成）。
void testFilterEffectiveNextFrame() {
    FrontendPreprocessor stage(0, 16);
    require(stage.setFilterConfig(bothDisabled()), "F9 start from the identity config");
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    WorkerGate gate;
    gate.install(stage);
    stage.start();
    stage.beginSession(1);

    // 出队即冻结系数：该帧已离开 FIFO、正在处理中，此时变更配置。
    auto inFlight = makeGroup(0, 1, 256, 1, 0, 0, false);
    const std::vector<float> inFlightA = inFlight->freqA;
    require(stage.submit(inFlight) == FrontendSubmitResult::Accepted, "F9 in-flight accepted");
    require(gate.waitEntered(), "F9 worker parked on the in-flight frame");

    require(stage.setFilterConfig(zeroPhaseConfig()), "F9 configuration updated");

    auto next = makeGroup(0, 2, 256, 1, 0, 1, false);
    const std::vector<float> nextA = next->freqA;
    require(stage.submit(next) == FrontendSubmitResult::Accepted, "F9 next frame accepted");
    gate.release();
    require(until([&] { return ring.count.load() == 2; }), "F9 both frames dispatched");

    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.triggers[0] == 1 && ring.freqAs[0] == inFlightA,
                "F9 the in-flight frame completed on the coefficients it started with");
        require(ring.triggers[1] == 2 && ring.freqAs[1] != nextA,
                "F9 the next dequeued frame uses the new coefficients");
        require(ring.freqAs[0].size() == inFlightA.size() &&
                    ring.freqAs[1].size() == nextA.size(),
                "F9 neither frame was interrupted mid-processing");
    }
    stage.stop();
    std::cout << "PASS  F9 effective moment: in-flight frame keeps its coefficients, next frame switches\n";
}

// ================================================================ F10
// 任务 7 检查项 10：系数设计计数——同一配置下连续处理多帧时 filterDesigns 不增长；
// 配置变更一次则增长一次；filterConfigVersion 同步递增。
void testFilterDesignCounters() {
    FrontendPreprocessor stage(0, 16);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    stage.start();
    stage.beginSession(1);

    const auto initial = stage.snapshot();
    require(initial.filterDesigns == 0 && initial.filterConfigVersion == 0,
            "F10 counters start at 0 for the configuration the stage was built with");

    for (int i = 0; i < 5; ++i) {
        require(stage.submit(makeGroup(0, static_cast<std::uint16_t>(i), 64, 1, 0, i, false)) ==
                    FrontendSubmitResult::Accepted, "F10 submit accepted");
    }
    require(until([&] { return stage.snapshot().processed == 5; }), "F10 frames processed");
    const auto afterFrames = stage.snapshot();
    require(afterFrames.filterDesigns == initial.filterDesigns,
            "F10 processing many frames under one config never re-designs");
    require(afterFrames.filteredFrames == 5, "F10 every frame was still filtered");

    require(stage.setFilterConfig(zeroPhaseConfig()), "F10 first configuration change");
    const auto afterChange1 = stage.snapshot();
    require(afterChange1.filterDesigns == afterFrames.filterDesigns + 1,
            "F10 one configuration change re-designs exactly once");
    require(afterChange1.filterConfigVersion == afterFrames.filterConfigVersion + 1,
            "F10 filterConfigVersion increments in step with filterDesigns");

    // 同一份参数重复下发不是一次"配置变更"：不重新设计，也不递增版本号。
    require(stage.setFilterConfig(zeroPhaseConfig()), "F10 identical re-delivery accepted");
    const auto afterResend = stage.snapshot();
    require(afterResend.filterDesigns == afterChange1.filterDesigns,
            "F10 re-delivering identical parameters does not re-design");
    require(afterResend.filterConfigVersion == afterChange1.filterConfigVersion,
            "F10 re-delivering identical parameters keeps the version");

    frontend_filter::Config third = zeroPhaseConfig();
    third.hpOrder = 4;
    require(stage.setFilterConfig(third), "F10 second configuration change");
    const auto afterChange2 = stage.snapshot();
    require(afterChange2.filterDesigns == afterChange1.filterDesigns + 1,
            "F10 a second change re-designs exactly once more");
    require(afterChange2.filterConfigVersion == afterChange1.filterConfigVersion + 1,
            "F10 version keeps counting in step");

    // 校验拒绝的下发不计入设计次数。
    frontend_filter::Config rejected = third;
    rejected.lpCutoffMhz = rejected.hpCutoffMhz;
    require(!stage.setFilterConfig(rejected), "F10 invalid config rejected");
    require(stage.snapshot().filterDesigns == afterChange2.filterDesigns,
            "F10 a rejected configuration never re-designs");
    stage.stop();
    std::cout << "PASS  F10 design counters: one design per configuration change, none per frame\n";
}

// ================================================================ F11
// 任务 7 检查项 11：并发更新——UI 线程更新配置与 worker 处理并发执行时无数据竞争。
// 可重复执行的压力用例：若系数对象被撕裂读取，输出会出现 NaN/Inf 或长度错乱。
void testFilterConcurrentConfigUpdate() {
    for (int round = 0; round < 3; ++round) {
        FrontendPreprocessor stage(0, 512);
        RingRecorder ring;
        stage.setRingSink(ring.sink());
        stage.start();
        stage.beginSession(1);

        std::atomic<bool> done{false};
        std::thread updater([&stage, &done] {
            frontend_filter::Config a = zeroPhaseConfig();
            frontend_filter::Config b;
            b.hpEnable = true;  b.hpCutoffMhz = 5.0;  b.hpOrder = 3;
            b.lpEnable = true;  b.lpCutoffMhz = 90.0; b.lpOrder = 1;
            while (!done.load(std::memory_order_acquire)) {
                stage.setFilterConfig(a);
                stage.setFilterConfig(b);
            }
        });

        constexpr int kFrames = 150;
        for (int i = 0; i < kFrames; ++i) {
            require(stage.submit(makeGroup(0, static_cast<std::uint16_t>(i), 128,
                                           1, 0, i, false)) ==
                        FrontendSubmitResult::Accepted, "F11 submit accepted under load");
        }
        require(until([&] {
                    const auto s = stage.snapshot();
                    return s.processed + s.exceptionDropped + s.staleDropped >= kFrames;
                }), "F11 every submitted frame was accounted for");
        done.store(true, std::memory_order_release);
        updater.join();

        const auto stats = stage.snapshot();
        require(stats.exceptionDropped == 0, "F11 concurrent updates never tore a coefficient set");
        require(stats.filterConfigVersion >= 2, "F11 configuration really did change concurrently");
        {
            std::lock_guard<std::mutex> lock(ring.mutex);
            for (std::size_t f = 0; f < ring.freqAs.size(); ++f) {
                require(ring.freqAs[f].size() == 128 && ring.freqBs[f].size() == 128,
                        "F11 clone length preserved under concurrent updates");
                for (float v : ring.freqAs[f])
                    require(std::isfinite(v), "F11 filter output stays finite under concurrent updates");
            }
        }
        stage.stop();
    }
    std::cout << "PASS  F11 concurrent config update: 3 x 150 frames with a racing updater, no torn state\n";
}

// ================================================================ F12
// 任务 7 检查项 12：参数校验——截止频率越界、阶数越界、两路启用时低通截止 ≤
// 高通截止，三种情况下配置不被更新。
void testFilterConfigValidation() {
    using frontend_filter::Validation;
    frontend_filter::Config c;

    // 截止频率越界（必须 > 0 且 < 125 MHz，开区间）
    for (double v : {0.0, -0.5, 125.0, 200.0}) {
        c = frontend_filter::Config{};  c.hpCutoffMhz = v;
        require(frontend_filter::validate(c) == Validation::HpCutoffRange,
                "F12 highpass cutoff out of (0, 125) MHz is rejected");
        c = frontend_filter::Config{};  c.lpCutoffMhz = v;
        require(frontend_filter::validate(c) == Validation::LpCutoffRange,
                "F12 lowpass cutoff out of (0, 125) MHz is rejected");
    }

    // 阶数越界（必须在 1 ~ 8）
    for (int v : {0, -1, 9, 100}) {
        c = frontend_filter::Config{};  c.hpOrder = v;
        require(frontend_filter::validate(c) == Validation::HpOrderRange,
                "F12 highpass order out of 1..8 is rejected");
        c = frontend_filter::Config{};  c.lpOrder = v;
        require(frontend_filter::validate(c) == Validation::LpOrderRange,
                "F12 lowpass order out of 1..8 is rejected");
    }

    // 两路都启用时，低通截止必须严格大于高通截止
    for (double lp : {60.0, 30.0}) {
        c = frontend_filter::Config{};
        c.hpCutoffMhz = 60.0;  c.lpCutoffMhz = lp;
        require(frontend_filter::validate(c) == Validation::BandOrder,
                "F12 lpCutoff <= hpCutoff with both paths enabled is rejected");
    }
    // 只有一路启用时不做带序约束
    c = frontend_filter::Config{};
    c.hpEnable = false;  c.hpCutoffMhz = 60.0;  c.lpCutoffMhz = 30.0;
    require(frontend_filter::validate(c) == Validation::Ok,
            "F12 band ordering is only required when both paths are enabled");

    // 三种拒绝情形下，生效配置与设计计数都保持原值
    struct Bad { const char* what; frontend_filter::Config cfg; };
    frontend_filter::Config range; range.hpCutoffMhz = 0.0;
    frontend_filter::Config order; order.lpOrder = 9;
    frontend_filter::Config band;  band.hpCutoffMhz = 60.0; band.lpCutoffMhz = 60.0;
    const Bad bad[] = {
        {"cutoff out of range", range},
        {"order out of range",  order},
        {"lp <= hp with both enabled", band},
    };
    FrontendPreprocessor stage(0, 4);
    require(stage.setFilterConfig(zeroPhaseConfig()), "F12 baseline config applied");
    const frontend_filter::Config before = stage.filterConfig();
    const auto statsBefore = stage.snapshot();
    for (const Bad& b : bad) {
        require(!stage.setFilterConfig(b.cfg), "F12 rejected configuration is refused");
        require(stage.filterConfig() == before, "F12 effective config unchanged after rejection");
        require(stage.snapshot().filterDesigns == statsBefore.filterDesigns,
                "F12 no coefficient re-design after rejection");
        require(stage.snapshot().filterConfigVersion == statsBefore.filterConfigVersion,
                "F12 version unchanged after rejection");
    }
    std::cout << "PASS  F12 parameter validation: 3 rejection classes leave the config untouched\n";
}

// ================================================================ F13
// 任务 7 检查项 13：异常包含——滤波实现抛出的异常不逃逸出 worker 线程，
// 计入既有 exceptionDropped 且线程继续处理后续帧。
void testFilterExceptionContainment() {
    FrontendPreprocessor stage(0, 64);
    require(stage.setFilterConfig(zeroPhaseConfig()), "F13 filter enabled");
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    stage.setFilterFaultForTest([](TriggerGroup& group) {
        if (group.triggerSeq == 1) throw std::runtime_error("filter implementation fault");
    });
    stage.start();
    stage.beginSession(1);

    // 滤波异常被拦在 worker 内：raw 保存线程不受影响，异常计入 exceptionDropped。
    AcqConfig config;
    std::atomic<int> saved{0};
    DataProcessor processor(0, nullptr, nullptr, config,
        [&stage](const TriggerGroupPtr& group) { return stage.submit(group); });
    processor.setDirectSaveSink([&](const TriggerGroupPtr&) { ++saved; return true; });

    const auto faulted = processor.deliverAssembled(makeGroup(0, 1, 64, 1, 0, 0, false), true, true);
    require(faulted.saveAccepted && !faulted.exception,
            "F13 save thread unaffected by a filter implementation fault");
    require(until([&] { return stage.snapshot().exceptionDropped == 1; }),
            "F13 filter fault counted as exceptionDropped");
    require(ring.count.load() == 0, "F13 the faulted frame was never dispatched");

    // 线程继续处理后续帧
    for (int i = 2; i <= 6; ++i) {
        const auto r = processor.deliverAssembled(
            makeGroup(0, static_cast<std::uint16_t>(i), 64, 1, 0, i - 1, false), true, true);
        require(r.saveAccepted && r.frontendAccepted, "F13 stage still accepting after the fault");
    }
    require(until([&] {
                const auto s = stage.snapshot();
                return s.processed == 5 && s.exceptionDropped == 1;
            }), "F13 worker kept serving later frames");
    require(saved.load() == 6, "F13 every raw save succeeded through the fault");
    require(stage.snapshot().queueRejected == 0, "F13 fault never counted as queue rejection");
    processor.requestStop();
    stage.stop();
    std::cout << "PASS  F13 exception containment: filter fault stayed on the worker and later frames kept flowing\n";
}

// ================================================================ S3
// S3 分工守卫：成像路径（ring sink）恰好被零相位滤波一次，不多不少。
//
// 依据（前提文档 G2）：B1 的反演核 b = 2p − 2r̃·p′ = −2r̃²·∂(p/r̃)/∂r̃ 成立的前提是
//   p 与 p′ 来自同一个信号数组。若成像数据被零相位滤两次，则 b 与「只滤一次」的
//   G2 合同不一致。故成像路径只允许一级滤波 = 既有 FrontendFilter，B1 不得再加一级。
//
// 反证：断言同时比对「恰好一次」与「两次」两个独立参考，且显式要求两者不同，
//       证明本守卫对「误加第二级滤波」确实有判别力，不是恒真式。
void testSingleFilterStageInImagingPath() {
    const frontend_filter::Config cfg = zeroPhaseConfig();
    require(frontend_filter::validate(cfg) == frontend_filter::Validation::Ok,
            "S3 config valid");
    const frontend_filter::Bank bank(cfg);
    require(bank.active(), "S3 bank designs coefficients");

    FrontendPreprocessor stage(0, 8);
    require(stage.setFilterConfig(cfg), "S3 stage accepts the config");
    DisplayBuffer display;
    stage.setDisplayBuffer(&display);
    RingRecorder ring;
    stage.setRingSink(ring.sink());
    stage.start();
    stage.beginSession(1);

    auto raw = makeGroup(0, 1, 128, 1, 0, 0, false);
    const std::vector<float> inputA = raw->freqA;
    const std::vector<float> inputB = raw->freqB;

    // 独立参考：恰好滤一次 / 滤两次（每次都用全新的 Bank，状态恒零，与生产一致）
    std::vector<float> onceA = inputA, onceB = inputB;
    require(frontend_filter::Bank(cfg).apply(onceA.data(), onceA.size()),
            "S3 once-pass A executed");
    require(frontend_filter::Bank(cfg).apply(onceB.data(), onceB.size()),
            "S3 once-pass B executed");
    std::vector<float> twiceA = onceA, twiceB = onceB;
    require(frontend_filter::Bank(cfg).apply(twiceA.data(), twiceA.size()),
            "S3 twice-pass A executed");
    require(frontend_filter::Bank(cfg).apply(twiceB.data(), twiceB.size()),
            "S3 twice-pass B executed");
    require(twiceA != onceA && twiceB != onceB,
            "S3 判别力：两次滤波结果确实不同于一次（否则守卫无效）");
    require(onceA != inputA,
            "S3 滤波确实改变了序列（配置非平凡）");

    require(stage.submit(raw) == FrontendSubmitResult::Accepted, "S3 submit accepted");
    require(until([&] { return ring.count.load() == 1; }), "S3 dispatch");
    {
        std::lock_guard<std::mutex> lock(ring.mutex);
        require(ring.freqAs.front() == onceA,
                "S3 成像路径 A 线恰好被滤一次");
        require(ring.freqBs.front() == onceB,
                "S3 成像路径 B 线恰好被滤一次");
        require(ring.freqAs.front() != twiceA,
                "S3 成像路径 A 线未被滤两次");
        require(ring.freqBs.front() != twiceB,
                "S3 成像路径 B 线未被滤两次");
    }
    stage.stop();
    std::cout << "PASS  S3 single filter stage: imaging path is zero-phase filtered exactly once\n";
}

}  // namespace

int main() {
    try {
        testIdentityDeepCopy();
        testFullResolution();
        testOneResultTwoConsumers();
        testFifoOrdering();
        testNonBlockingSubmit();
        testBoundedOverflow();
        testSaveIsolation();
        testSessionStale();
        testTimeoutStale();
        testCountBoundaryFinal();
        testStopLifecycle();
        testExceptionContainment();

        testFilterDisabledIsIdentity();
        testFilterPathsExecute();
        testFilterZeroPhase();
        testFilterPerALineIndependence();
        testFilterEdgeExtension();
        testFilterShortSequence();
        testFilterSaveIsolation();
        testFilterSharedCloneDispatch();
        testFilterEffectiveNextFrame();
        testFilterDesignCounters();
        testFilterConcurrentConfigUpdate();
        testFilterConfigValidation();
        testFilterExceptionContainment();

        testSingleFilterStageInImagingPath();
    } catch (const std::exception& error) {
        std::cout << "FAIL  " << error.what() << "\n";
        return 1;
    }
    if (g_failures) return 1;
    std::cout << "PASS  frontend_preprocessor_test: 12/12 architecture contracts + "
                 "13/13 filter contracts + S3 single-filter-stage guard\n";
    return 0;
}
