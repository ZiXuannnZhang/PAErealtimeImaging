#include "MultiPortReceiver.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTextStream>

namespace {
bool check(bool condition, const QString& message)
{
    if (!condition) QTextStream(stderr) << "FAIL " << message << Qt::endl;
    return condition;
}

AcqConfig testConfig()
{
    AcqConfig config;
    config.acqTimeNs = 4000; // six 32-bit A+B packets per trigger
    config.sampleIntervalNs = 4.0;
    config.bitsPerChannel = 32;
    config.displayPoints = 16;
    return config;
}

QByteArray datagram(uint16_t trigger, uint16_t sequence)
{
    QByteArray value(static_cast<int>(UDP_HEADER_BYTES + 8), '\0');
    value[0] = static_cast<char>(sequence & 0xff);
    value[1] = static_cast<char>((sequence >> 8) & 0xff);
    value[2] = static_cast<char>(trigger & 0xff);
    value[3] = static_cast<char>((trigger >> 8) & 0xff);
    return value;
}

bool waitForComplete(DataProcessor& processor, uint64_t expected, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (processor.statsSnapshot().triggersComplete >= expected) return true;
        QThread::msleep(2);
    }
    return processor.statsSnapshot().triggersComplete >= expected;
}

bool runBoundaryStress()
{
    const AcqConfig config = testConfig();
    DataProcessor processor(0, nullptr, nullptr, nullptr, config);
    MultiPortReceiver receiver({0}, {&processor}, -1, nullptr, true);
    processor.start();
    receiver.start();
    bool ok = check(receiver.waitUntilStarted(1000), QStringLiteral("receiver test seam started"));

    // Session A establishes both a partial assembly and a raw anchor.
    ok = check(receiver.prepareSession(1, 1000), QStringLiteral("session A receiver prepare")) && ok;
    ok = check(processor.prepareSession(1, 1000), QStringLiteral("session A processor prepare")) && ok;
    ok = check(receiver.armSession(1, 1000), QStringLiteral("session A receiver arm")) && ok;
    ok = check(processor.armSession(1, 1000), QStringLiteral("session A processor arm")) && ok;
    ok = check(receiver.commitSession(1, 1000), QStringLiteral("session A commit")) && ok;
    receiver.dispatchDatagramForTest(datagram(100, 0));
    QElapsedTimer partialWait;
    partialWait.start();
    while (processor.statsSnapshot().processorPacketsDequeued == 0 && partialWait.elapsed() < 1000)
        QThread::msleep(2);

    const auto before = processor.statsSnapshot();
    ok = check(before.rawSequenceInitialized && before.lastTriggerSeq == 100,
               QStringLiteral("session A raw anchor established")) && ok;

    // Stop boundary closes receiver admission before it drains trailing data.
    ok = check(receiver.disarmSession(1000), QStringLiteral("session A receiver disarm")) && ok;
    ok = check(processor.disarmSession(1000), QStringLiteral("session A processor disarm")) && ok;
    receiver.dispatchDatagramForTest(datagram(100, 1));

    // Session B: inject old packets during both ARMED phases.  The production
    // dispatch helper must count them at socket level but never observe/enqueue.
    ok = check(receiver.prepareSession(2, 1000), QStringLiteral("session B receiver prepare")) && ok;
    ok = check(processor.prepareSession(2, 1000), QStringLiteral("session B processor prepare")) && ok;
    ok = check(receiver.armSession(2, 1000), QStringLiteral("session B receiver arm")) && ok;
    receiver.dispatchDatagramForTest(datagram(100, 2));
    ok = check(processor.armSession(2, 1000), QStringLiteral("session B processor arm")) && ok;
    receiver.dispatchDatagramForTest(datagram(100, 3));
    const auto preCommit = processor.statsSnapshot();
    ok = check(!preCommit.rawSequenceInitialized,
               QStringLiteral("trailing packets do not rebuild raw anchor")) && ok;
    ok = check(receiver.commitSession(2, 1000), QStringLiteral("session B commit")) && ok;
    const int expectedPackets = config.packetsPerTrig();
    for (int sequence = 0; sequence < expectedPackets; ++sequence)
        receiver.dispatchDatagramForTest(datagram(700, static_cast<uint16_t>(sequence)));
    ok = check(waitForComplete(processor, 1, 2000),
               QStringLiteral("session B first trigger completes")) && ok;

    const auto afterFirst = processor.statsSnapshot();
    ok = check(afterFirst.packetsDropped == before.packetsDropped
                   && afterFirst.triggersPartial == before.triggersPartial
                   && afterFirst.staleTriggerPacketsDiscarded == before.staleTriggerPacketsDiscarded,
               QStringLiteral("receiver trailing packets do not create processor loss")) && ok;
    ok = check(afterFirst.rawSequenceInitialized && afterFirst.lastTriggerSeq == 700,
               QStringLiteral("session B raw anchor belongs to admitted packet")) && ok;

    // Repeat the real receiver prepare/arm/commit ordering for 100 sessions,
    // varying sequence wrap/jumps and injecting trailing data at each phase.
    for (int cycle = 0; cycle < 100; ++cycle) {
        const uint64_t token = static_cast<uint64_t>(cycle + 3);
        ok = check(receiver.disarmSession(1000), QStringLiteral("stress disarm %1").arg(cycle)) && ok;
        ok = check(processor.disarmSession(1000), QStringLiteral("stress processor disarm %1").arg(cycle)) && ok;
        ok = check(receiver.prepareSession(token, 1000), QStringLiteral("stress receiver prepare %1").arg(cycle)) && ok;
        ok = check(processor.prepareSession(token, 1000), QStringLiteral("stress processor prepare %1").arg(cycle)) && ok;
        ok = check(receiver.armSession(token, 1000), QStringLiteral("stress receiver arm %1").arg(cycle)) && ok;
        receiver.dispatchDatagramForTest(datagram(100, 8));
        ok = check(processor.armSession(token, 1000), QStringLiteral("stress processor arm %1").arg(cycle)) && ok;
        receiver.dispatchDatagramForTest(datagram(100, 9));
        ok = check(receiver.commitSession(token, 1000), QStringLiteral("stress commit %1").arg(cycle)) && ok;

        uint16_t trigger = 0;
        if (cycle == 0) trigger = 65534;
        else if (cycle == 1) trigger = 65535;
        else if (cycle == 2) trigger = 0;
        else if (cycle == 3) trigger = 400;
        else trigger = static_cast<uint16_t>(cycle * 977);
        for (int sequence = 0; sequence < expectedPackets; ++sequence)
            receiver.dispatchDatagramForTest(datagram(trigger, static_cast<uint16_t>(sequence)));
        ok = check(waitForComplete(processor, static_cast<uint64_t>(cycle + 2), 2000),
                   QStringLiteral("stress first trigger complete %1").arg(cycle)) && ok;
    }

    const auto stats = processor.statsSnapshot();
    ok = check(stats.triggersComplete == 101
                   && stats.packetsDropped == 0
                   && stats.triggersPartial == 0
                   && stats.staleTriggerPacketsDiscarded == 0,
               QStringLiteral("100 receiver boundary cycles have no cross-session loss")) && ok;
    ok = check(stats.sessionBoundaryPacketsDiscarded >= 202,
               QStringLiteral("trailing packets are visible as boundary discards")) && ok;
    ok = check(stats.socketPacketsReceived > stats.processorPacketsDequeued,
               QStringLiteral("socket cumulative receive includes closed-admission packets")) && ok;

    receiver.requestStop();
    processor.requestStop();
    receiver.wait(2000);
    processor.wait(2000);
    return ok;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const bool ok = runBoundaryStress();
    QTextStream(stdout) << (ok ? "PASS" : "FAIL")
                        << " receiver-aware session boundary stress" << Qt::endl;
    return ok ? 0 : 1;
}
