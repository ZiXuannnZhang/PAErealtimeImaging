// Raw-save isolation from the imaging branch under 4000-frame pressure.
//
// Task 1 architecture: DataProcessor submits to the per-card Frontend
// Preprocessing Stage; the stage worker feeds the Ring (ImagingBypass).  A
// saturated/rejecting downstream imaging queue must never prevent the raw save
// path from consuming a frame, and a frontend submit exception must not escape
// into the acquisition/save thread.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "DataProcessor.h"
#include "FrontendPreprocessor.h"
#include "ImagingBypass.h"

using namespace std::chrono_literals;

void require(bool v, const char* m) { if (!v) throw std::runtime_error(m); }

template <class P>
bool until(P p) {
    auto end = std::chrono::steady_clock::now() + 5s;
    while (!p() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    return p();
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    // Downstream imaging branch: tiny bounded queue with a deliberately delayed
    // worker, so it saturates exactly like the historical test.
    ImagingBypass bypass(8);
    std::array<bool, 8> channels{};
    channels[0] = channels[1] = true;
    bypass.setEnabledChannels(channels);
    bypass.setEnabled(true);
    bypass.setServiceReady(true);
    bypass.start();
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    std::atomic<int> entered{0}, saved{0};
    bypass.setConsumer([&](const TriggerGroupConstPtr&, const std::array<bool, 8>&) {
        ++entered;
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return release; });
        return true;
    });

    // Frontend Preprocessing Stage.  The queue is sized so no frontend admission
    // is refused: the pressure under test is the downstream imaging queue.
    FrontendPreprocessor stage(0, 8192);
    stage.setRingSink([&](const TriggerGroupConstPtr& frame) { return bypass.tryPush(frame); });
    stage.start();
    stage.beginSession(1);

    AcqConfig config;
    DataProcessor processor(0, nullptr, nullptr, config,
        [&stage](const TriggerGroupPtr& frame) { return stage.submit(frame); });
    processor.setDirectSaveSink([&](const TriggerGroupPtr&) { ++saved; return true; });

    int frontendAccepted = 0, frontendRejected = 0;
    for (int i = 0; i < 4000; ++i) {
        auto f = std::make_shared<TriggerGroup>();
        f->cardId = 0;
        f->triggerSeq = std::uint16_t(i);
        f->measurementSession = 1;
        f->isComplete = true;
        f->sampleCount = 32;
        f->freqA.assign(32, 1);
        f->freqB.assign(32, -1);
        auto r = processor.deliverAssembled(f, true, true);
        require(r.saveAccepted && r.save == DataProcessor::DeliveryResult::Consumed,
                "save acceptance");
        frontendAccepted += r.frontendAccepted ? 1 : 0;
        frontendRejected += r.frontendSubmit == FrontendSubmitResult::QueueFull ||
                            r.frontendSubmit == FrontendSubmitResult::QueueBusy;
    }

    // A throwing frontend submit sink must not escape into the save/acquisition
    // thread: the frame is still consumed by the raw save path.
    DataProcessor throwing(0, nullptr, nullptr, config,
        [](const TriggerGroupPtr&) -> FrontendSubmitResult {
            throw std::runtime_error("frontend");
        });
    throwing.setDirectSaveSink([&](const TriggerGroupPtr&) { ++saved; return true; });
    auto e = std::make_shared<TriggerGroup>();
    e->cardId = 0;
    e->isComplete = true;
    e->sampleCount = 1;
    e->freqA = {1};
    e->freqB = {-1};
    auto er = throwing.deliverAssembled(e, true, true);
    require(er.saveAccepted && er.exception && !er.frontendAccepted,
            "frontend submit exception isolated");

    require(until([&] { return bypass.snapshot().attempts == 4000; }),
            "frontend stage delivered every frame to the imaging branch");

    { std::lock_guard<std::mutex> lock(mutex); release = true; }
    cv.notify_all();
    require(until([&] { return entered.load() >= 1; }), "worker delay");
    bypass.stop();
    stage.stop();
    processor.requestStop();

    const auto imaging = bypass.snapshot();
    require(saved == 4001 && imaging.accepted <= 9 &&
                (imaging.droppedQueueFull + imaging.droppedQueueBusy) >= 3991,
            "4000-frame save/imaging isolation");
    require(frontendAccepted == 4000 && frontendRejected == 0,
            "frontend admission never leaked into save acceptance");
    std::cout << "PASS 4000-frame pressure: saveAccepted=4000 while the downstream imaging queue "
                 "saturated and its worker was delayed\n";
    return 0;
}
