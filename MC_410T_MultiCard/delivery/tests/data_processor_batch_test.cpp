#include "DataProcessor.h"
#include "MultiPortReceiver.h"
#include "Constants.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>
#include <QThread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {

bool check(bool condition, const QString& message)
{
    if (!condition)
        QTextStream(stderr) << "FAIL " << message << Qt::endl;
    return condition;
}

AcqConfig testConfig()
{
    AcqConfig config;
    config.acqTimeNs = 8;
    config.bitsPerChannel = 16;
    config.sampleIntervalNs = 4.0;
    return config;
}

DataPacket packet(uint16_t seq)
{
    DataPacket value;
    value.packetSeq = seq;
    value.triggerSeq = 0;
    value.dataSize = 4;
    return value;
}

bool testBatchBoundary513()
{
    DataProcessor processor(0, nullptr, nullptr, nullptr, testConfig());
    for (uint16_t i = 0; i < 513; ++i)
        processor.enqueuePacket(packet(i));

    const int first = processor.drainBatchForTest();
    const auto firstStats = processor.statsSnapshot();
    if (!check(first == PROC_BATCH_SIZE, QStringLiteral("513 first batch count"))) return false;
    if (!check(firstStats.processorPacketsDequeued == 512,
               QStringLiteral("513 first batch dequeued exactly 512"))) return false;
    if (!check(firstStats.inputQueueDepth == 1,
               QStringLiteral("513 boundary packet remained queued"))) return false;
    if (!check(firstStats.batchBoundaryDiscards == 0,
               QStringLiteral("513 batch boundary discard sentinel"))) return false;

    const int second = processor.drainBatchForTest();
    const auto finalStats = processor.statsSnapshot();
    return check(second == 1 && finalStats.processorPacketsDequeued == 513
                     && finalStats.packetsReceived == 513
                     && finalStats.inputQueueDepth == 0
                     && finalStats.batchBoundaryDiscards == 0,
                 QStringLiteral("513 second batch conservation"));
}

bool testExactBatch()
{
    DataProcessor processor(0, nullptr, nullptr, nullptr, testConfig());
    for (uint16_t i = 0; i < 512; ++i)
        processor.enqueuePacket(packet(i));
    const int drained = processor.drainBatchForTest();
    const auto stats = processor.statsSnapshot();
    return check(drained == 512 && stats.processorPacketsDequeued == 512
                     && stats.inputQueueDepth == 0
                     && stats.batchBoundaryDiscards == 0,
                 QStringLiteral("exact 512 batch"));
}

bool testBelowBatch()
{
    DataProcessor processor(0, nullptr, nullptr, nullptr, testConfig());
    for (uint16_t i = 0; i < 511; ++i)
        processor.enqueuePacket(packet(i));
    const int drained = processor.drainBatchForTest();
    const auto stats = processor.statsSnapshot();
    return check(drained == 511 && stats.processorPacketsDequeued == 511
                     && stats.inputQueueDepth == 0
                     && stats.batchBoundaryDiscards == 0,
                 QStringLiteral("below 512 batch"));
}

bool testMultiBatchConservation()
{
    DataProcessor processor(0, nullptr, nullptr, nullptr, testConfig());
    for (uint16_t i = 0; i < 1025; ++i)
        processor.enqueuePacket(packet(i));

    int total = 0;
    int rounds = 0;
    for (;;) {
        const int drained = processor.drainBatchForTest();
        if (drained == 0) break;
        if (!check(drained <= PROC_BATCH_SIZE, QStringLiteral("batch quota exceeded"))) return false;
        total += drained;
        ++rounds;
    }
    const auto stats = processor.statsSnapshot();
    return check(total == 1025 && rounds == 3
                     && stats.processorPacketsDequeued == 1025
                     && stats.packetsReceived == 1025
                     && stats.inputQueueDepth == 0
                     && stats.batchBoundaryDiscards == 0,
                 QStringLiteral("multi-batch conservation"));
}

bool testRawReceiveOrderTracker()
{
    CardStats sequential;
    sequential.observeRawReceive(9, 0);
    sequential.observeRawReceive(9, 1);
    sequential.observeRawReceive(9, 2);
    sequential.observeRawReceive(9, 3);
    const auto sequentialSnapshot = sequential.snapshot();
    bool ok = check(sequentialSnapshot.sameTriggerForwardGapEvents == 0
                        && sequentialSnapshot.sameTriggerForwardGapPackets == 0
                        && sequentialSnapshot.sameTriggerBackstepEvents == 0
                        && sequentialSnapshot.sameTriggerDuplicateSeqEvents == 0,
                    QStringLiteral("same-trigger sequential order has no anomaly"));

    CardStats stats;
    stats.observeRawReceive(10, 0);
    stats.observeRawReceive(10, 1);
    stats.observeRawReceive(10, 4);
    stats.observeRawReceive(10, 4);
    stats.observeRawReceive(10, 3);
    stats.observeRawReceive(11, 0);
    stats.observeRawReceive(10, 5);
    const auto snapshot = stats.snapshot();
    ok = check(snapshot.sameTriggerForwardGapEvents == 1
                   && snapshot.sameTriggerForwardGapPackets == 2,
               QStringLiteral("same-trigger forward gap classification")) && ok;
    ok = check(snapshot.sameTriggerDuplicateSeqEvents == 1
                   && snapshot.sameTriggerBackstepEvents == 1,
               QStringLiteral("same-trigger duplicate/backstep classification")) && ok;
    ok = check(snapshot.crossTriggerLateArrivalEvents == 1,
               QStringLiteral("cross-trigger late arrival classification")) && ok;

    CardStats packetWrap;
    packetWrap.observeRawReceive(20, 65535);
    packetWrap.observeRawReceive(20, 0);
    const auto packetWrapSnapshot = packetWrap.snapshot();
    ok = check(packetWrapSnapshot.sameTriggerForwardGapEvents == 0
                   && packetWrapSnapshot.sameTriggerBackstepEvents == 0,
               QStringLiteral("packet sequence uint16 wrap")) && ok;

    CardStats triggerWrap;
    triggerWrap.observeRawReceive(65535, 1);
    triggerWrap.observeRawReceive(0, 0);
    triggerWrap.observeRawReceive(65535, 2);
    const auto triggerWrapSnapshot = triggerWrap.snapshot();
    return check(triggerWrapSnapshot.crossTriggerLateArrivalEvents == 1,
                 QStringLiteral("trigger sequence uint16 wrap/late arrival")) && ok;
}

bool testAssemblyRejectionClassification()
{
    AcqConfig config = testConfig();
    config.acqTimeNs = 8000;
    DataProcessor processor(0, nullptr, nullptr, nullptr, config);
    processor.setMeasureEnabled(true);

    DataPacket accepted;
    accepted.triggerSeq = 0;
    accepted.packetSeq = 100;
    accepted.dataSize = 4;
    DataPacket duplicate = accepted;
    DataPacket outOfRange = accepted;
    outOfRange.packetSeq = 10;
    processor.enqueuePacket(accepted);
    processor.enqueuePacket(duplicate);
    processor.enqueuePacket(outOfRange);
    processor.drainBatchForTest();
    auto stats = processor.statsSnapshot();
    bool ok = check(stats.assemblyDuplicatePackets == 1,
                    QStringLiteral("assembly duplicate classification"));
    ok = check(stats.assemblyOffsetOutOfRangePackets == 1,
               QStringLiteral("assembly offset out-of-range classification")) && ok;

    DataPacket nextTrigger = accepted;
    nextTrigger.triggerSeq = 1;
    nextTrigger.packetSeq = 101;
    DataPacket stale = accepted;
    stale.packetSeq = 102;
    processor.enqueuePacket(nextTrigger);
    processor.enqueuePacket(stale);
    processor.drainBatchForTest();
    stats = processor.statsSnapshot();
    return check(stats.staleTriggerPacketsDiscarded == 1,
                 QStringLiteral("stale-trigger classification")) && ok;
}

// ── Session B：missingTriggerCount（跳号数）确定性测试 ─────────────────────
// 语义：只累计由 forward triggerSeq gap 推断的、完全 0 包到达的 missing
// trigger 数量；partial trigger 只计 triggersPartial + 包级 packetsDropped。
namespace {
DataPacket triggerPacket(uint16_t triggerSeq, uint16_t packetSeq)
{
    DataPacket pkt;
    pkt.triggerSeq = triggerSeq;
    pkt.packetSeq = packetSeq;
    pkt.dataSize = 4;
    return pkt;
}

// 6 packets/trigger：acqTimeNs=8000, 4ns 间隔, 16bit/通道 -> 8000B/触发 -> 6 包
AcqConfig sixPacketConfig()
{
    AcqConfig config = testConfig();
    config.acqTimeNs = 8000;
    return config;
}
}

// B1 触发切换路径：T100 部分到达(3/6) 后 T104 到达
//   missingTriggerCount += 3（仅一次，didSwitch 阻止空缓冲路径双计）
//   packetsDropped += 3（旧触发缺包） + 3 * expectedPackets（完整缺失触发包当量）
bool testMissingTriggerSwitchPath()
{
    AcqConfig config = sixPacketConfig();
    const int expectedPackets = config.packetsPerTrig();
    if (!check(expectedPackets == 6, QStringLiteral("B1 expected 6 packets/trigger")))
        return false;
    DataProcessor processor(0, nullptr, nullptr, nullptr, config);
    processor.setMeasureEnabled(true);

    for (uint16_t p = 0; p < 3; ++p)
        processor.enqueuePacket(triggerPacket(100, p));
    processor.enqueuePacket(triggerPacket(104, 0));
    processor.drainBatchForTest();
    const auto stats = processor.statsSnapshot();
    return check(stats.missingTriggerCount == 3
                     && stats.triggersPartial == 1
                     && stats.packetsDropped ==
                            static_cast<uint64_t>(3 + 3 * expectedPackets),
                 QStringLiteral("B1 T100->T104 switch: +3 missing triggers exactly once"));
}

// B2 仅 partial trigger：T200 收 4/6 包后切到相邻 T201（无序号缺口）
//   missingTriggerCount 不变；packetsDropped 只含 partial 内缺包
bool testMissingTriggerPartialOnly()
{
    AcqConfig config = sixPacketConfig();
    DataProcessor processor(0, nullptr, nullptr, nullptr, config);
    processor.setMeasureEnabled(true);

    for (uint16_t p = 0; p < 4; ++p)
        processor.enqueuePacket(triggerPacket(200, p));
    processor.enqueuePacket(triggerPacket(201, 0));
    processor.drainBatchForTest();
    const auto stats = processor.statsSnapshot();
    return check(stats.triggersPartial == 1
                     && stats.packetsDropped == 2
                     && stats.missingTriggerCount == 0,
                 QStringLiteral("B2 partial trigger does not increment missingTriggerCount"));
}

// B3 空缓冲区锚点路径：完整触发 flush 后缓冲区为空，下一触发跨 gap
//   T100(完成) -> T104 -> T108：每条路径按 trigger 数累计且不双计
bool testMissingTriggerEmptyBufferGapPath()
{
    DataProcessor processor(0, nullptr, nullptr, nullptr, testConfig());  // 1 包/触发
    processor.setMeasureEnabled(true);

    processor.enqueuePacket(triggerPacket(100, 0));   // 完成并 flush
    processor.enqueuePacket(triggerPacket(104, 0));   // gap=3：T101/T102/T103 完全缺失
    processor.enqueuePacket(triggerPacket(108, 0));   // gap=3：T105/T106/T107 完全缺失
    processor.drainBatchForTest();
    const auto stats = processor.statsSnapshot();
    return check(stats.missingTriggerCount == 6
                     && stats.packetsDropped == 6
                     && stats.triggersComplete == 3
                     && stats.triggersPartial == 0,
                 QStringLiteral("B3 empty-buffer gap accumulates per missing trigger"));
}

// B4 uint16 wrap / backstep / reset recovery
bool testMissingTriggerWrapForward()
{
    DataProcessor processor(0, nullptr, nullptr, nullptr, testConfig());  // 1 包/触发
    processor.setMeasureEnabled(true);

    processor.enqueuePacket(triggerPacket(65534, 0));  // 完成并 flush
    processor.enqueuePacket(triggerPacket(1, 0));      // wrap forward：T65535/T0 缺失
    processor.enqueuePacket(triggerPacket(0, 0));      // 小幅回退=迟到包：stale，不计跳号
    processor.drainBatchForTest();
    const auto stats = processor.statsSnapshot();
    return check(stats.missingTriggerCount == 2
                     && stats.packetsDropped == 2
                     && stats.triggersComplete == 2
                     && stats.staleTriggerPacketsDiscarded == 1,
                 QStringLiteral("B4 wrap-forward gap counts exactly, backstep excluded"));
}

bool testMissingTriggerResetRecovery()
{
    DataProcessor processor(0, nullptr, nullptr, nullptr, testConfig());  // 1 包/触发
    processor.setMeasureEnabled(true);

    processor.enqueuePacket(triggerPacket(1000, 0));  // 完成并 flush
    // 触发序号大幅回退（>= kTriggerResetBackJumpThreshold）：判定新一帧/新一轮，
    // 锚点重置并接受当前包，不产生跳号/丢包
    processor.enqueuePacket(triggerPacket(10, 0));
    processor.enqueuePacket(triggerPacket(9, 0));     // 小幅回退：stale，不计跳号
    processor.drainBatchForTest();
    const auto stats = processor.statsSnapshot();
    return check(stats.missingTriggerCount == 0
                     && stats.packetsDropped == 0
                     && stats.triggersComplete == 2
                     && stats.staleTriggerPacketsDiscarded == 1,
                 QStringLiteral("B4 reset recovery/backstep do not increment missingTriggerCount"));
}


#ifdef _WIN32
bool testSocketCounterBoundary()
{
    DataProcessor processor(0, nullptr, nullptr, nullptr, testConfig());
    std::vector<int> cardIndices{0};
    std::vector<DataProcessor*> processors{&processor};
    MultiPortReceiver receiver(cardIndices, processors, -1);
    receiver.start();
    if (!check(receiver.waitUntilStarted(2000), QStringLiteral("receiver start"))) {
        receiver.requestStop();
        receiver.wait(2000);
        return false;
    }
    // Legacy test path explicitly opts into compatibility admission; the
    // production controller uses prepare/arm/commit barriers instead.
    receiver.setCompatibilityAdmission(true);

    SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!check(sender != INVALID_SOCKET, QStringLiteral("UDP sender socket"))) {
        receiver.requestStop();
        receiver.wait(2000);
        return false;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(BASE_PORT);
    inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr);

    const QByteArray shortPacket(2, '\0');
    const QByteArray validPacket(5, '\0');
    const int shortSent = sendto(sender, shortPacket.constData(), shortPacket.size(), 0,
                                 reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    const int validSent = sendto(sender, validPacket.constData(), validPacket.size(), 0,
                                 reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    closesocket(sender);

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000 &&
           processor.stats().socketPacketsReceived.load(std::memory_order_relaxed) < 1)
        QThread::msleep(2);

    receiver.requestStop();
    receiver.wait(2000);
    const auto beforeDrain = processor.statsSnapshot();
    const auto ingress = receiver.observabilitySnapshot();
    const int drained = processor.drainBatchForTest();
    const auto afterDrain = processor.statsSnapshot();
    bool ok = check(shortSent == shortPacket.size() && validSent == validPacket.size()
                     && beforeDrain.socketPacketsReceived == 1
                     && beforeDrain.processorPacketsDequeued == 0
                     && drained == 1
                     && afterDrain.processorPacketsDequeued == 1,
                 QStringLiteral("socket counter accepts only valid data packets"));
    ok = check(ingress.selectWakeups > 0 && ingress.recvHardErrors == 0
                   && ingress.recvWouldBlockTerminations > 0
                   && ingress.maxDrainPackets >= 1,
               QStringLiteral("receiver select/drain observability")) && ok;
    return ok;
}
#endif

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = testBatchBoundary513();
    ok = testExactBatch() && ok;
    ok = testBelowBatch() && ok;
    ok = testMultiBatchConservation() && ok;
    ok = testRawReceiveOrderTracker() && ok;
    ok = testAssemblyRejectionClassification() && ok;
    ok = testMissingTriggerSwitchPath() && ok;
    ok = testMissingTriggerPartialOnly() && ok;
    ok = testMissingTriggerEmptyBufferGapPath() && ok;
    ok = testMissingTriggerWrapForward() && ok;
    ok = testMissingTriggerResetRecovery() && ok;
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        QTextStream(stderr) << "FAIL WSAStartup" << Qt::endl;
        ok = false;
    } else {
        ok = testSocketCounterBoundary() && ok;
        WSACleanup();
    }
#endif
    if (ok)
        QTextStream(stdout) << "PASS DataProcessor batch/counter tests" << Qt::endl;
    return ok ? 0 : 1;
}
