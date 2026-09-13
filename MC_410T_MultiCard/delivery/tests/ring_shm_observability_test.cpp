#include "ImagingSharedMemory.h"
#include "RingShmObservability.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void testProducerClean()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    const auto event = tracker.observeProducerSubmit(0, 7, 8, 1000);
    const auto snapshot = tracker.snapshot();
    check(!event.slotBusy && snapshot.submitted == 1 && snapshot.slotBusyBeforeSubmit == 0,
          "T1 clean producer submit");
}

void testProducerOverwrite()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    const auto event = tracker.observeProducerSubmit(1, 10, 11, 1000);
    const auto snapshot = tracker.snapshot();
    check(event.slotBusy && snapshot.slotBusyBeforeSubmit == 1,
          "T2 producer sees ready=1 overwrite");
}

void testConsumerMatch()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    tracker.observeNotification(10, 1);
    const auto first = tracker.observeConsumed(10, true, 10, 1, 100, 120, 3);
    tracker.observeNotification(11, 2);
    const auto second = tracker.observeConsumed(11, true, 11, 1, 200, 220, 4);
    const auto snapshot = tracker.snapshot();
    check(!first.hasAnomaly() && !second.hasAnomaly()
              && snapshot.notifyShmMismatch == 0 && snapshot.readyZeroBeforeCopy == 0
              && snapshot.duplicateShmSeq == 0 && snapshot.shmSeqGap == 0,
          "T3 matching consumer notification");
}

void testMismatch()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    const auto event = tracker.observeConsumed(10, true, 11, 1, 0, 0, 1);
    check(event.notifyShmMismatch && tracker.snapshot().notifyShmMismatch == 1,
          "T4 notify/shm sequence mismatch");
}

void testReadyZero()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    const auto event = tracker.observeConsumed(10, true, 10, 0, 0, 0, 1);
    check(event.readyZeroBeforeCopy && tracker.snapshot().readyZeroBeforeCopy == 1,
          "T5 ready zero before copy");
}

void testDuplicate()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    tracker.observeConsumed(10, true, 10, 1, 0, 0, 1);
    const auto event = tracker.observeConsumed(10, true, 10, 1, 0, 0, 1);
    check(event.duplicateShmSeq && tracker.snapshot().duplicateShmSeq == 1,
          "T6 duplicate sequence");
}

void testGap()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    tracker.observeConsumed(10, true, 10, 1, 0, 0, 1);
    const auto event = tracker.observeConsumed(12, true, 12, 1, 0, 0, 1);
    check(event.shmSeqGap && tracker.snapshot().shmSeqGap == 1,
          "T7 sequence gap");
}

void testResetContinuity()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    tracker.observeConsumed(30, true, 30, 1, 0, 0, 1);
    tracker.resetEpoch();
    const auto event = tracker.observeConsumed(0, true, 0, 1, 0, 0, 1);
    check(!event.shmSeqGap && tracker.snapshot().epoch == 1,
          "T8 reset starts a new epoch");
}

void testWrap()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    tracker.observeConsumed(std::numeric_limits<uint32_t>::max(), true,
                            std::numeric_limits<uint32_t>::max(), 1, 0, 0, 1);
    const auto event = tracker.observeConsumed(0, true, 0, 1, 0, 0, 1);
    check(!event.shmSeqGap && tracker.snapshot().shmSeqGap == 0,
          "T9 uint32 sequence wrap");
}

void testLegacyReadyMessage()
{
    const QJsonObject oldMessage = QJsonDocument::fromJson(
        QByteArrayLiteral("{\"cmd\":\"ring_block_ready\",\"seq\":1}"))
        .object();
    const auto oldReady = ring_shm_obs::parseReadyMessage(oldMessage);
    check(oldReady.hasSeq && oldReady.seq == 1 && oldReady.submitIndex == 0
              && oldReady.submitWallUs == 0,
          "T10 old ready message remains valid");

    QJsonObject extended = oldMessage;
    extended[QStringLiteral("submit_index")] = 3;
    extended[QStringLiteral("submit_wall_us")] = 100;
    const auto parsed = ring_shm_obs::parseReadyMessage(extended);
    check(parsed.submitIndex == 3 && parsed.submitWallUs == 100,
          "T10 optional ready metadata");
}

void testHeaderAbi()
{
    static_assert(sizeof(RingImagingShmHeader) == 64,
                  "RingImagingShmHeader ABI changed");
    check(sizeof(RingImagingShmHeader) == 64, "T11 header remains 64 bytes");
}

void testDeterministicMismatch()
{
    ring_shm_obs::Tracker tracker;
    tracker.beginSession();
    tracker.observeNotification(41, 9);
    const auto event = tracker.observeConsumed(41, true, 42, 1, 500, 525, 7);
    const auto snapshot = tracker.snapshot();
    check(event.hasAnomaly() && event.notifyShmMismatch
              && snapshot.notifyShmMismatch == 1 && snapshot.notifications == 1
              && snapshot.consumed == 1,
          "T13 deterministic mismatch path");
}

} // namespace

int main()
{
    testProducerClean();
    testProducerOverwrite();
    testConsumerMatch();
    testMismatch();
    testReadyZero();
    testDuplicate();
    testGap();
    testResetContinuity();
    testWrap();
    testLegacyReadyMessage();
    testHeaderAbi();
    testDeterministicMismatch();
    std::puts("ring_shm_observability_test: PASS (T1-T11,T13)");
    return 0;
}
