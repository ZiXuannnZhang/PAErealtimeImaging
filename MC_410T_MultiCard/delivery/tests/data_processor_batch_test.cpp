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
    config.displayPoints = 1;
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
    const int drained = processor.drainBatchForTest();
    const auto afterDrain = processor.statsSnapshot();
    return check(shortSent == shortPacket.size() && validSent == validPacket.size()
                     && beforeDrain.socketPacketsReceived == 1
                     && beforeDrain.processorPacketsDequeued == 0
                     && drained == 1
                     && afterDrain.processorPacketsDequeued == 1,
                 QStringLiteral("socket counter accepts only valid data packets"));
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
