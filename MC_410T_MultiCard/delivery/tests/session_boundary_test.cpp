#include "DataProcessor.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTextStream>

namespace {
bool check(bool condition, const QString &message)
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
    return config;
}

DataPacket packet(uint16_t trigger, uint16_t sequence)
{
    DataPacket value;
    value.triggerSeq = trigger;
    value.packetSeq = sequence;
    value.dataSize = 8;
    return value;
}

bool waitForComplete(DataProcessor &processor, uint64_t expected, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (processor.statsSnapshot().triggersComplete >= expected) return true;
        QThread::msleep(2);
    }
    return processor.statsSnapshot().triggersComplete >= expected;
}

bool testResetAndFirstTrigger()
{
    const AcqConfig config = testConfig();
    DataProcessor processor(0, nullptr, nullptr, config);
    processor.start();
    processor.setMeasureEnabled(true); // create a pre-session partial assembly
    processor.enqueuePacket(packet(100, 0));
    QElapsedTimer wait;
    wait.start();
    while (processor.sessionStateForTest().assemblyReceived == 0 && wait.elapsed() < 1000)
        QThread::msleep(2);
    const auto before = processor.statsSnapshot();
    const bool prepared = processor.prepareSession(1, 2000);
    const auto resetState = processor.sessionStateForTest();
    bool ok = check(prepared, QStringLiteral("session prepare barrier"));
    ok = check(resetState.assemblyReceived == 0 && !resetState.hasFlushedOnce
                   && resetState.activeSessionToken == 0 && !resetState.measureEnabled,
               QStringLiteral("partial assembly and anchors reset")) && ok;

    processor.enqueuePacket(packet(500, 0)); // gate-off packet must not enter next session
    ok = check(processor.armSession(1, 2000), QStringLiteral("session arm barrier")) && ok;
    const int expectedPackets = config.packetsPerTrig();
    for (int sequence = 0; sequence < expectedPackets; ++sequence)
        processor.enqueuePacket(packet(500, static_cast<uint16_t>(sequence)));
    ok = check(waitForComplete(processor, 1, 2000),
               QStringLiteral("first armed trigger completes")) && ok;
    const auto after = processor.statsSnapshot();
    ok = check(after.packetsDropped == before.packetsDropped
                   && after.triggersPartial == before.triggersPartial,
               QStringLiteral("session reset does not create packet loss or partial trigger")) && ok;
    ok = check(after.staleTriggerPacketsDiscarded == 0,
               QStringLiteral("old queue packet does not become stale new-session data")) && ok;
    processor.disarmSession(2000);
    processor.requestStop();
    processor.wait(2000);
    return ok;
}

bool testRepeatedBoundaries()
{
    const AcqConfig config = testConfig();
    DataProcessor processor(0, nullptr, nullptr, config);
    processor.start();
    const int expectedPackets = config.packetsPerTrig();
    constexpr int cycles = 100;
    bool ok = true;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        const uint64_t token = static_cast<uint64_t>(cycle + 10);
        ok = check(processor.prepareSession(token, 2000),
                   QStringLiteral("prepare cycle %1").arg(cycle)) && ok;
        ok = check(processor.armSession(token, 2000),
                   QStringLiteral("arm cycle %1").arg(cycle)) && ok;
        uint16_t trigger = 0;
        if (cycle < 2) trigger = static_cast<uint16_t>(65534 + cycle);
        else if (cycle == 2) trigger = 0;
        else if (cycle == 3) trigger = 400;
        else trigger = static_cast<uint16_t>(cycle * 97);
        for (int sequence = 0; sequence < expectedPackets; ++sequence)
            processor.enqueuePacket(packet(trigger, static_cast<uint16_t>(sequence)));
        ok = check(waitForComplete(processor, static_cast<uint64_t>(cycle + 1), 2000),
                   QStringLiteral("complete first trigger cycle %1").arg(cycle)) && ok;
        ok = check(processor.disarmSession(2000),
                   QStringLiteral("disarm cycle %1").arg(cycle)) && ok;
    }
    const auto stats = processor.statsSnapshot();
    ok = check(stats.triggersComplete == cycles && stats.packetsDropped == 0
                   && stats.triggersPartial == 0
                   && stats.staleTriggerPacketsDiscarded == 0,
               QStringLiteral("100 session boundaries have no cross-session loss")) && ok;
    processor.requestStop();
    processor.wait(2000);
    return ok;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    bool ok = testResetAndFirstTrigger();
    ok = testRepeatedBoundaries() && ok;
    QTextStream(stdout) << (ok ? "PASS" : "FAIL")
                        << " session boundary reset/100-cycle tests" << Qt::endl;
    return ok ? 0 : 1;
}
