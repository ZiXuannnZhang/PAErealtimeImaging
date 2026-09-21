#include "MultiPortReceiver.h"
#include "MeasurementSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTextStream>
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

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

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) return true;
        QThread::msleep(1);
    }
    return predicate();
}

#ifdef _WIN32
class LoopbackUdpSender final
{
public:
    LoopbackUdpSender()
    {
        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }

    ~LoopbackUdpSender()
    {
        if (m_socket != INVALID_SOCKET) closesocket(m_socket);
    }

    bool isValid() const { return m_socket != INVALID_SOCKET; }

    bool sendPacket(int cardIndex, const QByteArray& packet) const
    {
        if (!isValid()) return false;
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(static_cast<u_short>(BASE_PORT + cardIndex));
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int sent = sendto(m_socket, packet.constData(), packet.size(), 0,
                                reinterpret_cast<const sockaddr*>(&destination),
                                sizeof(destination));
        return sent == packet.size();
    }

private:
    SOCKET m_socket = INVALID_SOCKET;
};

class ReceiverFixture final
{
public:
    ReceiverFixture(int cardCount, bool realSockets, const AcqConfig& config)
    {
        for (int card = 0; card < cardCount; ++card) {
            m_owned.push_back(std::make_unique<DataProcessor>(
                card, nullptr, nullptr, nullptr, config));
            m_processors.push_back(m_owned.back().get());
            m_cards.push_back(card);
        }
        m_receiver = std::make_unique<MultiPortReceiver>(
            m_cards, m_processors, -1, nullptr, !realSockets);
    }

    ~ReceiverFixture() { shutdown(); }

    bool start()
    {
        for (auto& processor : m_owned) processor->start();
        m_receiver->start();
        return m_receiver->waitUntilStarted(1500);
    }

    bool startSession(uint64_t token)
    {
        if (!m_receiver->prepareSession(token, 1500)) return false;
        for (auto& processor : m_owned)
            if (!processor->prepareSession(token, 1500)) return false;
        if (!m_receiver->armSession(token, 1500)) return false;
        for (auto& processor : m_owned)
            if (!processor->armSession(token, 1500)) return false;
        return true;
    }

    bool stopSession()
    {
        m_receiver->setCommandProcessingBlockedForTest(false);
        bool ok = m_receiver->disarmSession(1500);
        for (auto& processor : m_owned)
            ok = processor->disarmSession(1500) && ok;
        return ok;
    }

    MultiPortReceiver& receiver() { return *m_receiver; }
    DataProcessor& processor(int index) { return *m_owned[static_cast<size_t>(index)]; }
    int cardCount() const { return static_cast<int>(m_owned.size()); }

    void shutdown()
    {
        if (!m_receiver) return;
        m_receiver->setCommandProcessingBlockedForTest(false);
        m_receiver->requestStop();
        for (auto& processor : m_owned) processor->requestStop();
        m_receiver->wait(2000);
        for (auto& processor : m_owned) processor->wait(2000);
        m_receiver.reset();
    }

private:
    std::vector<std::unique_ptr<DataProcessor>> m_owned;
    std::vector<DataProcessor*> m_processors;
    std::vector<int> m_cards;
    std::unique_ptr<MultiPortReceiver> m_receiver;
};

bool sendTrigger(LoopbackUdpSender& sender, int cardIndex,
                 uint16_t trigger, int packetsPerTrigger)
{
    for (int sequence = 0; sequence < packetsPerTrigger; ++sequence) {
        if (!sender.sendPacket(cardIndex,
                               datagram(trigger, static_cast<uint16_t>(sequence))))
            return false;
    }
    return true;
}

bool runLoopbackExactWindow()
{
    const AcqConfig config = testConfig();
    const int packetsPerTrigger = config.packetsPerTrig();
    ReceiverFixture fixture(1, true, config);
    LoopbackUdpSender sender;
    bool ok = check(sender.isValid(), QStringLiteral("loopback sender created"));
    ok = check(fixture.start(), QStringLiteral("loopback receiver started")) && ok;
    const uint64_t token = 1001;
    ok = check(fixture.startSession(token), QStringLiteral("exact-window session armed")) && ok;

    // T4: a trailing packet visible before begin is consumed by the closed
    // path and cannot establish the new processor raw anchor.
    const auto beforeTrailing = fixture.processor(0).statsSnapshot();
    ok = check(sender.sendPacket(0, datagram(99, 55)),
               QStringLiteral("send pre-Start trailing packet")) && ok;
    ok = check(waitUntil([&] {
                   return fixture.processor(0).statsSnapshot().socketPacketsReceived
                       >= beforeTrailing.socketPacketsReceived + 1;
               }, 1000), QStringLiteral("pre-Start trailing packet drained")) && ok;
    const auto afterTrailing = fixture.processor(0).statsSnapshot();
    ok = check(afterTrailing.socketPacketsReceived == beforeTrailing.socketPacketsReceived + 1
                   && afterTrailing.processorPacketsDequeued == beforeTrailing.processorPacketsDequeued
                   && afterTrailing.sessionBoundaryPacketsDiscarded
                          == beforeTrailing.sessionBoundaryPacketsDiscarded + 1
                   && !afterTrailing.rawSequenceInitialized,
               QStringLiteral("pre-Start trailing packet remains closed-admission")) && ok;

    ok = check(fixture.receiver().beginCardStartFence(token, 0, 1500),
               QStringLiteral("T2 begin fence")) && ok;
    ok = check(fixture.receiver().cardAdmissionState(0)
                   == MultiPortReceiver::AdmissionState::StartFenceHold,
               QStringLiteral("T2 receiver enters START_FENCE_HOLD")) && ok;
    ok = check(fixture.receiver().admissionState()
                   == MultiPortReceiver::AdmissionState::StartFenceHold,
               QStringLiteral("T2 aggregate exposes START_FENCE_HOLD")) && ok;

    const auto beforeHeld = fixture.processor(0).statsSnapshot();
    ok = check(sendTrigger(sender, 0, 700, packetsPerTrigger),
               QStringLiteral("T2 send first trigger into held backlog")) && ok;
    QThread::msleep(50);
    const auto whileHeld = fixture.processor(0).statsSnapshot();
    ok = check(whileHeld.socketPacketsReceived == beforeHeld.socketPacketsReceived
                   && whileHeld.processorPacketsDequeued == beforeHeld.processorPacketsDequeued
                   && whileHeld.sessionBoundaryPacketsDiscarded
                          == beforeHeld.sessionBoundaryPacketsDiscarded
                   && !whileHeld.rawSequenceInitialized,
               QStringLiteral("T2 HOLD prevents recv dispatch and raw admission")) && ok;

    ok = check(fixture.receiver().completeCardStartFence(token, 0, true, 1500),
               QStringLiteral("T2 complete fence success")) && ok;
    ok = check(waitForComplete(fixture.processor(0), 1, 2000),
               QStringLiteral("T2 same held datagrams complete first trigger")) && ok;
    const auto afterHeld = fixture.processor(0).statsSnapshot();
    ok = check(afterHeld.socketPacketsReceived == beforeHeld.socketPacketsReceived + packetsPerTrigger
                   && afterHeld.processorPacketsDequeued
                          == beforeHeld.processorPacketsDequeued + packetsPerTrigger
                   && afterHeld.sessionBoundaryPacketsDiscarded
                          == beforeHeld.sessionBoundaryPacketsDiscarded
                   && afterHeld.packetsDropped == beforeHeld.packetsDropped
                   && afterHeld.triggersPartial == beforeHeld.triggersPartial
                   && afterHeld.staleTriggerPacketsDiscarded
                          == beforeHeld.staleTriggerPacketsDiscarded
                   && afterHeld.rawSequenceInitialized
                   && afterHeld.lastTriggerSeq == 700,
               QStringLiteral("T2 first admitted trigger has zero loss")) && ok;

    // T5: local Start failure closes and clears a held backlog; it never
    // reaches processor/raw-order, and a later clean session is usable.
    ok = check(fixture.stopSession(), QStringLiteral("T5 stop first session")) && ok;
    const uint64_t failedToken = 1002;
    ok = check(fixture.startSession(failedToken), QStringLiteral("T5 failure session armed")) && ok;
    ok = check(fixture.receiver().beginCardStartFence(failedToken, 0, 1500),
               QStringLiteral("T5 begin fence")) && ok;
    const auto beforeFailure = fixture.processor(0).statsSnapshot();
    ok = check(sendTrigger(sender, 0, 710, packetsPerTrigger),
               QStringLiteral("T5 send held failure backlog")) && ok;
    QThread::msleep(30);
    const bool completedFailure = fixture.receiver().completeCardStartFence(
        failedToken, 0, false, 1500);
    ok = check(completedFailure, QStringLiteral("T5 complete fence failure cleanup")) && ok;
    const auto afterFailure = fixture.processor(0).statsSnapshot();
    ok = check(fixture.receiver().cardAdmissionState(0)
                   == MultiPortReceiver::AdmissionState::Disarmed,
               QStringLiteral("T5 failed Start never enters RUNNING")) && ok;
    ok = check(afterFailure.socketPacketsReceived
                       == beforeFailure.socketPacketsReceived + packetsPerTrigger
                   && afterFailure.processorPacketsDequeued
                          == beforeFailure.processorPacketsDequeued
                   && afterFailure.sessionBoundaryPacketsDiscarded
                          == beforeFailure.sessionBoundaryPacketsDiscarded + packetsPerTrigger
                   && !afterFailure.rawSequenceInitialized,
               QStringLiteral("T5 failure cleanup keeps backlog out of processor")) && ok;

    ok = check(fixture.stopSession(), QStringLiteral("T5 cleanup failed session")) && ok;
    const uint64_t cleanToken = 1003;
    ok = check(fixture.startSession(cleanToken), QStringLiteral("T5 clean retry armed")) && ok;
    ok = check(fixture.receiver().beginCardStartFence(cleanToken, 0, 1500),
               QStringLiteral("T5 clean retry begin")) && ok;
    ok = check(sendTrigger(sender, 0, 720, packetsPerTrigger),
               QStringLiteral("T5 clean retry first trigger")) && ok;
    ok = check(fixture.receiver().completeCardStartFence(cleanToken, 0, true, 1500),
               QStringLiteral("T5 clean retry complete")) && ok;
    ok = check(waitForComplete(fixture.processor(0), 2, 2000),
               QStringLiteral("T5 clean retry completes first trigger")) && ok;
    ok = check(fixture.stopSession(), QStringLiteral("exact-window fixture stop")) && ok;
    return ok;
}

bool runLoopbackPendingCompletion()
{
    const AcqConfig config = testConfig();
    const int packetsPerTrigger = config.packetsPerTrig();
    ReceiverFixture fixture(1, true, config);
    LoopbackUdpSender sender;
    bool ok = check(sender.isValid(), QStringLiteral("pending sender created"));
    ok = check(fixture.start(), QStringLiteral("pending receiver started")) && ok;
    const uint64_t token = 1101;
    ok = check(fixture.startSession(token), QStringLiteral("pending session armed")) && ok;
    ok = check(fixture.receiver().beginCardStartFence(token, 0, 1500),
               QStringLiteral("T3 begin fence")) && ok;

    fixture.receiver().setCommandProcessingBlockedForTest(true);
    std::atomic<bool> completionReturned{false};
    bool completionResult = false;
    std::thread completion([&] {
        completionResult = fixture.receiver().completeCardStartFence(token, 0, true, 1500);
        completionReturned.store(true, std::memory_order_release);
    });
    QThread::msleep(20);
    const auto before = fixture.processor(0).statsSnapshot();
    ok = check(sendTrigger(sender, 0, 1100, packetsPerTrigger),
               QStringLiteral("T3 send readable data with complete pending")) && ok;
    QThread::msleep(50);
    const auto whilePending = fixture.processor(0).statsSnapshot();
    ok = check(!completionReturned.load(std::memory_order_acquire),
               QStringLiteral("T3 complete command remains pending")) && ok;
    ok = check(whilePending.socketPacketsReceived == before.socketPacketsReceived
                   && whilePending.processorPacketsDequeued == before.processorPacketsDequeued
                   && whilePending.sessionBoundaryPacketsDiscarded
                          == before.sessionBoundaryPacketsDiscarded
                   && !whilePending.rawSequenceInitialized,
               QStringLiteral("T3 readable HOLD socket is not drained")) && ok;

    fixture.receiver().setCommandProcessingBlockedForTest(false);
    completion.join();
    ok = check(completionResult, QStringLiteral("T3 pending complete succeeds")) && ok;
    ok = check(waitForComplete(fixture.processor(0), 1, 2000),
               QStringLiteral("T3 pending data completes after RUNNING publish")) && ok;
    const auto after = fixture.processor(0).statsSnapshot();
    ok = check(after.socketPacketsReceived == before.socketPacketsReceived + packetsPerTrigger
                   && after.processorPacketsDequeued
                          == before.processorPacketsDequeued + packetsPerTrigger
                   && after.sessionBoundaryPacketsDiscarded
                          == before.sessionBoundaryPacketsDiscarded
                   && after.packetsDropped == before.packetsDropped
                   && after.triggersPartial == before.triggersPartial
                   && after.staleTriggerPacketsDiscarded
                          == before.staleTriggerPacketsDiscarded,
               QStringLiteral("T3 same readable batch is admitted after completion")) && ok;
    fixture.stopSession();
    return ok;
}

bool runLoopbackFourCardFence()
{
    const AcqConfig config = testConfig();
    const int packetsPerTrigger = config.packetsPerTrig();
    ReceiverFixture fixture(4, true, config);
    LoopbackUdpSender sender;
    bool ok = check(sender.isValid(), QStringLiteral("four-card sender created"));
    ok = check(fixture.start(), QStringLiteral("four-card loopback receiver started")) && ok;
    const uint64_t token = 1201;
    ok = check(fixture.startSession(token), QStringLiteral("four-card loopback armed")) && ok;

    std::array<uint64_t, 4> completeCounts{};
    for (int card = 0; card < 4; ++card) {
        const auto before = fixture.processor(card).statsSnapshot();
        ok = check(fixture.receiver().beginCardStartFence(token, card, 1500),
                   QStringLiteral("T6 card %1 begin HOLD").arg(card)) && ok;
        ok = check(fixture.receiver().cardAdmissionState(card)
                       == MultiPortReceiver::AdmissionState::StartFenceHold,
                   QStringLiteral("T6 card %1 is held").arg(card)) && ok;
        ok = check(fixture.receiver().admissionState()
                       == MultiPortReceiver::AdmissionState::StartFenceHold,
                   QStringLiteral("T6 aggregate exposes card %1 HOLD").arg(card)) && ok;
        for (int other = 0; other < card; ++other)
            ok = check(fixture.receiver().cardAdmissionState(other)
                           == MultiPortReceiver::AdmissionState::Running,
                       QStringLiteral("T6 earlier card %1 remains RUNNING").arg(other)) && ok;
        for (int other = card + 1; other < 4; ++other)
            ok = check(fixture.receiver().cardAdmissionState(other)
                           == MultiPortReceiver::AdmissionState::Armed,
                       QStringLiteral("T6 later card %1 remains ARMED").arg(other)) && ok;

        const uint16_t heldTrigger = static_cast<uint16_t>(1300 + card);
        ok = check(sendTrigger(sender, card, heldTrigger, packetsPerTrigger),
                   QStringLiteral("T6 card %1 send held trigger").arg(card)) && ok;
        QThread::msleep(30);
        const auto held = fixture.processor(card).statsSnapshot();
        ok = check(held.socketPacketsReceived == before.socketPacketsReceived
                       && held.processorPacketsDequeued == before.processorPacketsDequeued
                       && held.sessionBoundaryPacketsDiscarded
                              == before.sessionBoundaryPacketsDiscarded,
                   QStringLiteral("T6 card %1 held socket is not drained").arg(card)) && ok;

        if (card > 0) {
            const auto runningBefore = fixture.processor(0).statsSnapshot();
            ok = check(sendTrigger(sender, 0,
                                   static_cast<uint16_t>(1300 + card), packetsPerTrigger),
                       QStringLiteral("T6 running card drains during card %1 HOLD").arg(card)) && ok;
            ok = check(waitForComplete(fixture.processor(0),
                                       completeCounts[0] + 1, 2000),
                       QStringLiteral("T6 running card completes during HOLD")) && ok;
            completeCounts[0] = fixture.processor(0).statsSnapshot().triggersComplete;
            ok = check(fixture.processor(0).statsSnapshot().socketPacketsReceived
                           >= runningBefore.socketPacketsReceived + packetsPerTrigger,
                       QStringLiteral("T6 running card socket counter advances")) && ok;
        }

        ok = check(fixture.receiver().completeCardStartFence(token, card, true, 1500),
                   QStringLiteral("T6 card %1 complete fence").arg(card)) && ok;
        ok = check(waitForComplete(fixture.processor(card), completeCounts[card] + 1, 2000),
                   QStringLiteral("T6 card %1 held batch completes").arg(card)) && ok;
        completeCounts[card] = fixture.processor(card).statsSnapshot().triggersComplete;
    }
    ok = check(fixture.receiver().admissionState()
                   == MultiPortReceiver::AdmissionState::Running,
               QStringLiteral("T6 aggregate RUNNING only after four completions")) && ok;
    for (int card = 0; card < 4; ++card) {
        const auto stats = fixture.processor(card).statsSnapshot();
        ok = check(stats.packetsDropped == 0 && stats.triggersPartial == 0
                       && stats.staleTriggerPacketsDiscarded == 0,
                   QStringLiteral("T6 card %1 has no receive loss").arg(card)) && ok;
    }
    fixture.stopSession();
    return ok;
}

bool runLoopbackExactWindowStress()
{
    const AcqConfig config = testConfig();
    const int packetsPerTrigger = config.packetsPerTrig();
    ReceiverFixture fixture(1, true, config);
    LoopbackUdpSender sender;
    bool ok = check(sender.isValid(), QStringLiteral("stress sender created"));
    ok = check(fixture.start(), QStringLiteral("stress loopback receiver started")) && ok;

    for (int cycle = 0; cycle < 100; ++cycle) {
        const uint64_t token = static_cast<uint64_t>(2000 + cycle);
        const uint16_t trigger = cycle == 0
            ? static_cast<uint16_t>(65534)
            : cycle == 1
                ? static_cast<uint16_t>(65535)
                : cycle == 2
                    ? static_cast<uint16_t>(0)
                    : cycle == 3
                        ? static_cast<uint16_t>(400)
                        : static_cast<uint16_t>(cycle * 977);
        ok = check(fixture.startSession(token),
                   QStringLiteral("T8 cycle %1 session armed").arg(cycle)) && ok;
        ok = check(fixture.receiver().beginCardStartFence(token, 0, 1500),
                   QStringLiteral("T8 cycle %1 begin HOLD").arg(cycle)) && ok;

        fixture.receiver().setCommandProcessingBlockedForTest(true);
        std::atomic<bool> completionReturned{false};
        bool completionResult = false;
        std::thread completion([&] {
            completionResult = fixture.receiver().completeCardStartFence(token, 0, true, 1500);
            completionReturned.store(true, std::memory_order_release);
        });
        QThread::msleep(5);
        const auto before = fixture.processor(0).statsSnapshot();
        ok = check(sendTrigger(sender, 0, trigger, packetsPerTrigger),
                   QStringLiteral("T8 cycle %1 send pending first trigger").arg(cycle)) && ok;
        QThread::msleep(5);
        const auto whilePending = fixture.processor(0).statsSnapshot();
        ok = check(!completionReturned.load(std::memory_order_acquire)
                       && whilePending.socketPacketsReceived == before.socketPacketsReceived
                       && whilePending.processorPacketsDequeued == before.processorPacketsDequeued
                       && whilePending.sessionBoundaryPacketsDiscarded
                              == before.sessionBoundaryPacketsDiscarded,
                   QStringLiteral("T8 cycle %1 exact pending/readable window held").arg(cycle)) && ok;
        fixture.receiver().setCommandProcessingBlockedForTest(false);
        completion.join();
        ok = check(completionResult,
                   QStringLiteral("T8 cycle %1 complete fence succeeds").arg(cycle)) && ok;
        ok = check(waitForComplete(fixture.processor(0),
                                   static_cast<uint64_t>(cycle + 1), 2000),
                   QStringLiteral("T8 cycle %1 first trigger completes").arg(cycle)) && ok;
        ok = check(fixture.stopSession(),
                   QStringLiteral("T8 cycle %1 stop/disarm").arg(cycle)) && ok;
    }

    const auto stats = fixture.processor(0).statsSnapshot();
    ok = check(stats.triggersComplete == 100
                   && stats.packetsDropped == 0
                   && stats.triggersPartial == 0
                   && stats.staleTriggerPacketsDiscarded == 0
                   && stats.sessionBoundaryPacketsDiscarded == 0
                   && stats.socketPacketsReceived
                          == static_cast<uint64_t>(100 * packetsPerTrigger)
                   && stats.processorPacketsDequeued
                          == static_cast<uint64_t>(100 * packetsPerTrigger),
               QStringLiteral("T8 100 exact-window cycles have zero loss")) && ok;
    return ok;
}
#else
bool runLoopbackExactWindow() { return true; }
bool runLoopbackPendingCompletion() { return true; }
bool runLoopbackFourCardFence() { return true; }
bool runLoopbackExactWindowStress() { return true; }
#endif

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
    ok = check(receiver.beginCardStartFence(1, 0, 1000), QStringLiteral("session A fence begin")) && ok;
    ok = check(receiver.completeCardStartFence(1, 0, true, 1000),
               QStringLiteral("session A fence complete")) && ok;
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
    // The local card Start send is the only event that may publish this fence.
    ok = check(receiver.beginCardStartFence(2, 0, 1000), QStringLiteral("session B fence begin")) && ok;
    ok = check(receiver.completeCardStartFence(2, 0, true, 1000),
               QStringLiteral("session B fence complete")) && ok;
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
        ok = check(receiver.beginCardStartFence(token, 0, 1000),
                   QStringLiteral("stress fence begin %1").arg(cycle)) && ok;
        ok = check(receiver.completeCardStartFence(token, 0, true, 1000),
                   QStringLiteral("stress fence complete %1").arg(cycle)) && ok;

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

bool runPerCardFenceOrdering()
{
    const AcqConfig config = testConfig();
    std::vector<std::unique_ptr<DataProcessor>> owned;
    std::vector<DataProcessor*> processors;
    for (int card = 0; card < 4; ++card) {
        owned.push_back(std::make_unique<DataProcessor>(card, nullptr, nullptr, nullptr, config));
        processors.push_back(owned.back().get());
    }
    MultiPortReceiver receiver({0, 1, 2, 3}, processors, -1, nullptr, true);
    for (auto& processor : owned) processor->start();
    receiver.start();
    bool ok = check(receiver.waitUntilStarted(1000), QStringLiteral("four-card receiver seam started"));
    const uint64_t token = 41;
    ok = check(receiver.prepareSession(token, 1000), QStringLiteral("four-card prepare")) && ok;
    for (auto& processor : owned)
        ok = check(processor->prepareSession(token, 1000), QStringLiteral("four-card processor prepare")) && ok;
    ok = check(receiver.armSession(token, 1000), QStringLiteral("four-card arm")) && ok;
    for (auto& processor : owned)
        ok = check(processor->armSession(token, 1000), QStringLiteral("four-card processor arm")) && ok;

    std::array<uint64_t, 4> dequeuedBefore{};
    for (int card = 0; card < 4; ++card) {
        dequeuedBefore[card] = owned[card]->statsSnapshot().processorPacketsDequeued;
        receiver.dispatchDatagramForTest(datagram(500, static_cast<uint16_t>(card)), card);
    }
    QThread::msleep(40);
    for (int card = 0; card < 4; ++card)
        ok = check(owned[card]->statsSnapshot().processorPacketsDequeued == dequeuedBefore[card],
                   QStringLiteral("card %1 remains closed before its fence").arg(card)) && ok;

    for (int card = 0; card < 4; ++card) {
        ok = check(receiver.beginCardStartFence(token, card, 1000),
                   QStringLiteral("card %1 fence begins").arg(card)) && ok;
        ok = check(receiver.cardAdmissionState(card)
                       == MultiPortReceiver::AdmissionState::StartFenceHold,
                   QStringLiteral("card %1 enters HOLD").arg(card)) && ok;
        ok = check(receiver.completeCardStartFence(token, card, true, 1000),
                   QStringLiteral("card %1 fence publishes").arg(card)) && ok;
        receiver.dispatchDatagramForTest(datagram(600, static_cast<uint16_t>(card)), card);
        QElapsedTimer timer;
        timer.start();
        while (owned[card]->statsSnapshot().processorPacketsDequeued <= dequeuedBefore[card]
               && timer.elapsed() < 1000)
            QThread::msleep(2);
        ok = check(owned[card]->statsSnapshot().processorPacketsDequeued > dequeuedBefore[card],
                   QStringLiteral("card %1 admits packet immediately after fence").arg(card)) && ok;
    }
    ok = check(receiver.admissionState() == MultiPortReceiver::AdmissionState::Running,
               QStringLiteral("aggregate receiver runs only after all four fences")) && ok;

    const auto stats = owned[0]->statsSnapshot();
    ok = check(stats.sessionBoundaryPacketsDiscarded >= 1,
               QStringLiteral("pre-fence packet is visible as boundary discard")) && ok;
    ok = check(receiver.disarmSession(1000), QStringLiteral("four-card receiver disarm")) && ok;
    for (auto& processor : owned)
        ok = check(processor->disarmSession(1000), QStringLiteral("four-card processor disarm")) && ok;
    receiver.requestStop();
    for (auto& processor : owned) processor->requestStop();
    receiver.wait(2000);
    for (auto& processor : owned) processor->wait(2000);
    return ok;
}

bool runPartialFenceRollback()
{
    const AcqConfig config = testConfig();
    std::vector<std::unique_ptr<DataProcessor>> owned;
    std::vector<DataProcessor*> processors;
    for (int card = 0; card < 4; ++card) {
        owned.push_back(std::make_unique<DataProcessor>(card, nullptr, nullptr, nullptr, config));
        processors.push_back(owned.back().get());
    }
    MultiPortReceiver receiver({0, 1, 2, 3}, processors, -1, nullptr, true);
    for (auto& processor : owned) processor->start();
    receiver.start();
    bool ok = check(receiver.waitUntilStarted(1000), QStringLiteral("rollback receiver seam started"));
    const uint64_t token = 51;
    bool rollbackCalled = false;
    const auto result = MeasurementSessionTransaction::start(
        4, 4,
        [&receiver, &owned, token](int index) {
            if (index == 0 && !receiver.prepareSession(token, 1000)) return false;
            return owned[index]->prepareSession(token, 1000);
        },
        [&receiver, &owned, token](int index) {
            if (index == 0 && !receiver.armSession(token, 1000)) return false;
            return owned[index]->armSession(token, 1000);
        },
        [](int cardIndex) {
            return cardIndex < 3
                ? MeasurementSessionTransaction::SendResult{1, 0}
                : MeasurementSessionTransaction::SendResult{0, 1};
        },
        [&receiver, &owned, &rollbackCalled]() {
            rollbackCalled = true;
            receiver.disarmSession(1000);
            for (auto& processor : owned) processor->disarmSession(1000);
        },
        {},
        [&receiver, token](int cardIndex) {
            return receiver.beginCardStartFence(token, cardIndex, 1000);
        },
        [&receiver, token](int cardIndex, bool startSucceeded) {
            return receiver.completeCardStartFence(token, cardIndex,
                                                   startSucceeded, 1000);
        });
    ok = check(!result.success && result.successCount == 3 && result.failCount == 1,
               QStringLiteral("three-card start followed by card3 failure rolls back")) && ok;
    ok = check(rollbackCalled && result.reason == QStringLiteral("partial_start_send"),
               QStringLiteral("partial fence transaction has rollback reason")) && ok;

    std::array<uint64_t, 4> beforeRollbackPackets{};
    for (int card = 0; card < 4; ++card) {
        beforeRollbackPackets[card] = owned[card]->statsSnapshot().processorPacketsDequeued;
        receiver.dispatchDatagramForTest(datagram(700, static_cast<uint16_t>(card)), card);
    }
    QThread::msleep(40);
    for (int card = 0; card < 4; ++card)
        ok = check(owned[card]->statsSnapshot().processorPacketsDequeued == beforeRollbackPackets[card],
                   QStringLiteral("rollback closes card %1 fence").arg(card)) && ok;

    const uint64_t cleanToken = 52;
    ok = check(receiver.prepareSession(cleanToken, 1000), QStringLiteral("clean retry prepare")) && ok;
    for (auto& processor : owned)
        ok = check(processor->prepareSession(cleanToken, 1000), QStringLiteral("clean retry processor prepare")) && ok;
    ok = check(receiver.armSession(cleanToken, 1000), QStringLiteral("clean retry arm")) && ok;
    for (auto& processor : owned)
        ok = check(processor->armSession(cleanToken, 1000), QStringLiteral("clean retry processor arm")) && ok;
    for (int card = 0; card < 4; ++card) {
        ok = check(receiver.beginCardStartFence(cleanToken, card, 1000),
                   QStringLiteral("clean retry fence begin %1").arg(card)) && ok;
        ok = check(receiver.completeCardStartFence(cleanToken, card, true, 1000),
                   QStringLiteral("clean retry fence complete %1").arg(card)) && ok;
    }
    const int packetsPerTrigger = config.packetsPerTrig();
    for (int card = 0; card < 4; ++card)
        for (int sequence = 0; sequence < packetsPerTrigger; ++sequence)
            receiver.dispatchDatagramForTest(datagram(800, static_cast<uint16_t>(sequence)), card);
    for (auto& processor : owned)
        ok = check(waitForComplete(*processor, 1, 2000), QStringLiteral("clean retry first trigger")) && ok;

    receiver.disarmSession(1000);
    for (auto& processor : owned) processor->disarmSession(1000);
    receiver.requestStop();
    for (auto& processor : owned) processor->requestStop();
    receiver.wait(2000);
    for (auto& processor : owned) processor->wait(2000);
    return ok;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 2;
#endif
    bool ok = runBoundaryStress();
    ok = runPerCardFenceOrdering() && ok;
    ok = runPartialFenceRollback() && ok;
    ok = runLoopbackExactWindow() && ok;
    ok = runLoopbackPendingCompletion() && ok;
    ok = runLoopbackFourCardFence() && ok;
    ok = runLoopbackExactWindowStress() && ok;
    QTextStream(stdout) << (ok ? "PASS" : "FAIL")
                        << " receiver-aware session boundary stress" << Qt::endl;
#ifdef _WIN32
    WSACleanup();
#endif
    return ok ? 0 : 1;
}
