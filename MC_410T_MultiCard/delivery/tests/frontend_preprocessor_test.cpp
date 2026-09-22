// Frontend Preprocessing Stage — Task 1 (identity processing) contract tests.
//
// Covers the twelve required cases: identity deep-copy, full-resolution,
// one-result/two-consumers, FIFO ordering, non-blocking submit, bounded
// overflow, save isolation, session stale, timeout stale, CountBoundary final,
// stop lifecycle and exception containment.
//
// No filter numeric result is asserted here: Butterworth/SOS/filtfilt are out of
// scope for Task 1 and processFrontendSignal() must stay identity.
//
// Determinism: the FRONTEND_PREPROCESSOR_TEST_SEAM worker seam parks the stage
// worker after it has dequeued exactly one frame.  That frame is therefore
// "in-flight" (already dequeued, not yet dispatched) and everything submitted
// afterwards stays "queued", which is exactly the state the stale-barrier and
// bounded-overflow contracts have to handle.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
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
            ++count;
            return ImagingSubmitResult::Accepted;
        };
    }
};

// ---------------------------------------------------------------- 1
void testIdentityDeepCopy() {
    FrontendPreprocessor stage(0, 8);
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
    } catch (const std::exception& error) {
        std::cout << "FAIL  " << error.what() << "\n";
        return 1;
    }
    if (g_failures) return 1;
    std::cout << "PASS  frontend_preprocessor_test: 12/12 Task 1 architecture contracts\n";
    return 0;
}
