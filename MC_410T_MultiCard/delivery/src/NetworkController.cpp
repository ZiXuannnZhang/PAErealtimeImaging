#include "NetworkController.h"
#include "FileSaver.h"
#include "NetworkDiagnostics.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QJsonArray>
#include <QJsonValue>
#include <QTimer>
#include <QDebug>
#include <QMetaObject>
#include <QThreadPool>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <limits>
#ifdef _WIN32
#include <iphlpapi.h>
#include <icmpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif


#ifdef _WIN32
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

static uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

namespace {

DiagnosticRecorder *diagnosticRecorder()
{
    return DiagnosticRecorder::instance();
}

} // namespace

NetworkController::NetworkController(QObject* parent)
    : QObject(parent)
    , m_controlSocket(INVALID_SOCKET)
    , m_feedbackSocket(INVALID_SOCKET)
{
    m_autoSaveCoordinator.setEventSink([this](
        const paimage::AutoSaveRoundCoordinator::Event& event) {
        QString message;
        DiagnosticRecorder::Severity severity = DiagnosticRecorder::Severity::Info;
        switch (event.kind) {
        case paimage::AutoSaveRoundCoordinator::Event::Kind::Reserved:
            message = QStringLiteral("auto_save_round_generation_reserved");
            break;
        case paimage::AutoSaveRoundCoordinator::Event::Kind::Committed:
            message = QStringLiteral("auto_save_round_generation_committed");
            break;
        case paimage::AutoSaveRoundCoordinator::Event::Kind::AlreadyApplied:
            message = QStringLiteral("auto_save_round_generation_already_applied");
            break;
        case paimage::AutoSaveRoundCoordinator::Event::Kind::Failed:
            message = QStringLiteral("auto_save_round_generation_failed");
            severity = DiagnosticRecorder::Severity::Error;
            break;
        }
        recordDiagnosticEvent(
            QStringLiteral("paimage.auto_save"), message, severity,
            {{QStringLiteral("measurementSession"),
              QString::number(event.measurementSession)},
             {QStringLiteral("roundGeneration"),
              QString::number(event.roundGeneration)},
             {QStringLiteral("boundaryKind"),
              QString::fromLatin1(paimage::autoSaveBoundaryKindName(event.boundaryKind))},
             {QStringLiteral("boundaryKey"), event.boundaryKey},
             {QStringLiteral("oldSessionGen"),
              QString::number(event.oldSessionGen)},
             {QStringLiteral("newSessionGen"),
              QString::number(event.newSessionGen)},
             {QStringLiteral("publishedSessionGen"),
              QString::number(event.publishedSessionGen)},
             {QStringLiteral("directory"), event.directory},
             {QStringLiteral("phase"), event.phase},
             {QStringLiteral("error"), event.error}});
    });
}

void NetworkController::setDiagnosticContext(const QString& listenId,
                                              const QString& source)
{
    m_diagnosticListenId = listenId;
    m_diagnosticSource = source.isEmpty() ? QStringLiteral("unknown") : source;

    QJsonObject fields;
    fields.insert(QStringLiteral("listenId"), m_diagnosticListenId);
    fields.insert(QStringLiteral("source"), m_diagnosticSource);
    recordDiagnosticEvent(QStringLiteral("network.context"),
                          QStringLiteral("diagnostic_context_set"),
                          DiagnosticRecorder::Severity::Info,
                          fields);
}

void NetworkController::recordDiagnosticEvent(const QString& category,
                                              const QString& message,
                                              DiagnosticRecorder::Severity severity,
                                              const QJsonObject& fields) const
{
    DiagnosticRecorder *recorder = diagnosticRecorder();
    if (!recorder) return;

    QJsonObject enriched = fields;
    if (!enriched.contains(QStringLiteral("listenId")))
        enriched.insert(QStringLiteral("listenId"), m_diagnosticListenId);
    if (!enriched.contains(QStringLiteral("source")))
        enriched.insert(QStringLiteral("source"), m_diagnosticSource);
    recorder->recordEvent(category, message, severity, enriched);
}

QString NetworkController::cardDiagnosticState(int cardIdx) const
{
    if (m_configPhase == ConfigPhase::Failed) {
        if (cardIdx >= 0 && cardIdx < static_cast<int>(m_configAck.size()) &&
            m_configAck[cardIdx])
            return QStringLiteral("confirmed");
        return QStringLiteral("failed");
    }
    if (m_configPhase == ConfigPhase::Confirmed)
        return QStringLiteral("confirmed");
    if (m_configPhase == ConfigPhase::WaitingAck) {
        if (cardIdx >= 0 && cardIdx < static_cast<int>(m_configAck.size()) &&
            m_configAck[cardIdx])
            return QStringLiteral("confirmed");
        return QStringLiteral("waiting_ack");
    }
    if (cardIdx >= 0 && cardIdx < static_cast<int>(m_cardsReady.size()) &&
        m_cardsReady[cardIdx])
        return QStringLiteral("ready");
    return QStringLiteral("idle");
}

QJsonObject NetworkController::runtimeStatsFields(const CardStats::Snapshot& stats)
{
    QJsonObject fields;
    fields.insert(QStringLiteral("packetsReceived"),
                  static_cast<double>(stats.packetsReceived));
    fields.insert(QStringLiteral("packetsDropped"),
                  static_cast<double>(stats.packetsDropped));
    fields.insert(QStringLiteral("triggersComplete"),
                  static_cast<double>(stats.triggersComplete));
    fields.insert(QStringLiteral("triggersPartial"),
                  static_cast<double>(stats.triggersPartial));
    fields.insert(QStringLiteral("triggersDiscarded"),
                  static_cast<double>(stats.triggersDiscarded));
    fields.insert(QStringLiteral("saveQueueDiscards"),
                  static_cast<double>(stats.saveQueueDiscards));
    fields.insert(QStringLiteral("inputQueueDepth"), stats.inputQueueDepth);
    fields.insert(QStringLiteral("saveQueueDepth"), stats.saveQueueDepth);
    fields.insert(QStringLiteral("recvMbps"), stats.recvMbps);
    fields.insert(QStringLiteral("triggerHz"), stats.triggerHz);
    fields.insert(QStringLiteral("packetLossRate"), stats.packetLossRate);
    fields.insert(QStringLiteral("socketPacketsReceived"),
                  static_cast<double>(stats.socketPacketsReceived));
    fields.insert(QStringLiteral("processorPacketsDequeued"),
                  static_cast<double>(stats.processorPacketsDequeued));
    fields.insert(QStringLiteral("batchBoundaryDiscards"),
                  static_cast<double>(stats.batchBoundaryDiscards));
    fields.insert(QStringLiteral("sameTriggerForwardGapEvents"),
                  static_cast<double>(stats.sameTriggerForwardGapEvents));
    fields.insert(QStringLiteral("sameTriggerForwardGapPackets"),
                  static_cast<double>(stats.sameTriggerForwardGapPackets));
    fields.insert(QStringLiteral("sameTriggerBackstepEvents"),
                  static_cast<double>(stats.sameTriggerBackstepEvents));
    fields.insert(QStringLiteral("sameTriggerDuplicateSeqEvents"),
                  static_cast<double>(stats.sameTriggerDuplicateSeqEvents));
    fields.insert(QStringLiteral("crossTriggerLateArrivalEvents"),
                  static_cast<double>(stats.crossTriggerLateArrivalEvents));
    fields.insert(QStringLiteral("staleTriggerPacketsDiscarded"),
                  static_cast<double>(stats.staleTriggerPacketsDiscarded));
    fields.insert(QStringLiteral("assemblyDuplicatePackets"),
                  static_cast<double>(stats.assemblyDuplicatePackets));
    fields.insert(QStringLiteral("assemblyOffsetOutOfRangePackets"),
                  static_cast<double>(stats.assemblyOffsetOutOfRangePackets));
    fields.insert(QStringLiteral("sessionBoundaryPacketsDiscarded"),
                  static_cast<double>(stats.sessionBoundaryPacketsDiscarded));
    fields.insert(QStringLiteral("lastTriggerSeq"), static_cast<int>(stats.lastTriggerSeq));
    fields.insert(QStringLiteral("lastPacketSeq"), static_cast<int>(stats.lastPacketSeq));
    fields.insert(QStringLiteral("rawSequenceInitialized"), stats.rawSequenceInitialized);
    return fields;
}

void NetworkController::recordCardSnapshots(const QString& stateOverride)
{
    DiagnosticRecorder *recorder = diagnosticRecorder();
    if (!recorder) return;

    QString phase = QStringLiteral("idle");
    if (m_configPhase == ConfigPhase::WaitingAck)
        phase = QStringLiteral("waiting_ack");
    else if (m_configPhase == ConfigPhase::Confirmed)
        phase = QStringLiteral("confirmed");
    else if (m_configPhase == ConfigPhase::Failed)
        phase = QStringLiteral("failed");

    const int count = std::min<int>(m_targetIPs.size(),
                                    static_cast<int>(m_cardDiagnostics.size()));
    for (int i = 0; i < count; ++i) {
        const CardDiagnosticState& card = m_cardDiagnostics[i];
        QJsonObject fields;
        fields.insert(QStringLiteral("listenId"), m_diagnosticListenId);
        fields.insert(QStringLiteral("configId"), m_currentConfigId);
        fields.insert(QStringLiteral("measurementSessionId"), m_measurementSessionId);
        fields.insert(QStringLiteral("source"), m_diagnosticSource);
        fields.insert(QStringLiteral("phase"), phase);
        fields.insert(QStringLiteral("discovery"), card.discovery);
        fields.insert(QStringLiteral("localAddress"), card.localAddress);
        fields.insert(QStringLiteral("readyPacketCount"),
                      static_cast<double>(card.readyPacketCount));
        fields.insert(QStringLiteral("arpReady"),
                      static_cast<double>(card.arpReady));
        fields.insert(QStringLiteral("sendCount"),
                      static_cast<double>(card.sendCount));
        fields.insert(QStringLiteral("ackPacketCount"),
                      static_cast<double>(card.ackPacketCount));
        fields.insert(QStringLiteral("retryCount"),
                      static_cast<double>(card.retryCount));
        fields.insert(QStringLiteral("retryResetCount"),
                      static_cast<double>(card.retryResetCount));
        fields.insert(QStringLiteral("cardIndex"), i + 1);

        if (const auto stats = getCardStats(i)) {
            const QJsonObject runtimeFields = runtimeStatsFields(*stats);
            for (auto it = runtimeFields.constBegin(); it != runtimeFields.constEnd(); ++it)
                fields.insert(it.key(), it.value());
        }

        const QString state = stateOverride.isEmpty()
            ? cardDiagnosticState(i)
            : stateOverride;
        recorder->setCardSnapshot(i + 1, m_targetIPs[i], state, fields);
    }
}

void NetworkController::scheduleNetworkSnapshot(const QString& reason,
                                                 const QString& phase,
                                                 const QString& configId)
{
    DiagnosticRecorder *recorder = diagnosticRecorder();
    if (!recorder) return;

    const QStringList targets = m_targetIPs.toList();
    const QString listenId = m_diagnosticListenId;
    const QString source = m_diagnosticSource;
    const QString reasonCopy = reason;
    const QString phaseCopy = phase;
    const QString configIdCopy = configId;
    QJsonObject config;
    config.insert(QStringLiteral("nCards"), m_config.nCards);
    config.insert(QStringLiteral("localBindIP"),
                  QString::fromStdString(m_config.localBindIP));
    config.insert(QStringLiteral("listenId"), listenId);
    config.insert(QStringLiteral("source"), source);
    config.insert(QStringLiteral("measurementSessionId"), m_measurementSessionId);

    (void)QtConcurrent::run([recorder, targets, listenId, source, reasonCopy,
                       phaseCopy, configIdCopy, config]() {
        QJsonObject snapshot = NetworkDiagnostics::collectSnapshot(targets);
        snapshot.insert(QStringLiteral("listenId"), listenId);
        snapshot.insert(QStringLiteral("source"), source);
        snapshot.insert(QStringLiteral("reason"), reasonCopy);
        snapshot.insert(QStringLiteral("phase"), phaseCopy);
        snapshot.insert(QStringLiteral("configId"), configIdCopy);
        snapshot.insert(QStringLiteral("measurementSessionId"),
                        config.value(QStringLiteral("measurementSessionId")));
        snapshot.insert(QStringLiteral("config"), config);
        QJsonArray orderedTargets;
        for (const QString& target : targets)
            orderedTargets.append(target);
        snapshot.insert(QStringLiteral("targetIPs"), orderedTargets);
        recorder->recordNetworkSnapshot(snapshot);
    });
}

void NetworkController::recordIngressSnapshot(const QString& reason)
{
    DiagnosticRecorder *recorder = diagnosticRecorder();
    if (!recorder) return;

    QJsonObject snapshot = m_ingressSampler.sample(m_targetIPs.toList());
    snapshot.insert(QStringLiteral("listenId"), m_diagnosticListenId);
    snapshot.insert(QStringLiteral("source"), m_diagnosticSource);
    snapshot.insert(QStringLiteral("reason"), reason);
    snapshot.insert(QStringLiteral("configId"), m_currentConfigId);
    snapshot.insert(QStringLiteral("measurementSessionId"), m_measurementSessionId);
    snapshot.insert(QStringLiteral("targetIPs"), QJsonArray::fromStringList(m_targetIPs.toList()));

    QJsonArray receiverGroups;
    for (const auto& receiver : m_receivers) {
        if (!receiver) continue;
        const auto stats = receiver->observabilitySnapshot();
        QJsonObject group;
        QJsonArray cards;
        for (const int card : stats.cardIndices) cards.append(card);
        group.insert(QStringLiteral("cardIndices"), cards);
        group.insert(QStringLiteral("selectWakeups"), static_cast<double>(stats.selectWakeups));
        group.insert(QStringLiteral("selectTimeouts"), static_cast<double>(stats.selectTimeouts));
        group.insert(QStringLiteral("selectErrors"), static_cast<double>(stats.selectErrors));
        group.insert(QStringLiteral("recvHardErrors"), static_cast<double>(stats.recvHardErrors));
        group.insert(QStringLiteral("recvWouldBlockTerminations"),
                     static_cast<double>(stats.recvWouldBlockTerminations));
        group.insert(QStringLiteral("maxDrainPackets"), static_cast<double>(stats.maxDrainPackets));
        group.insert(QStringLiteral("maxDrainDurationUs"), static_cast<double>(stats.maxDrainDurationUs));
        group.insert(QStringLiteral("maxReceiverLoopGapUs"),
                     static_cast<double>(stats.maxReceiverLoopGapUs));
        const auto admission = receiver->admissionState();
        group.insert(QStringLiteral("sessionAdmissionState"),
                     admission == MultiPortReceiver::AdmissionState::Running
                         ? QStringLiteral("running")
                         : admission == MultiPortReceiver::AdmissionState::StartFenceHold
                             ? QStringLiteral("start_fence_hold")
                             : admission == MultiPortReceiver::AdmissionState::Armed
                                 ? QStringLiteral("armed")
                                 : admission == MultiPortReceiver::AdmissionState::Preparing
                                     ? QStringLiteral("preparing")
                                     : QStringLiteral("disarmed"));
        QJsonArray socketStats;
        for (const auto& socket : stats.sockets) {
            QJsonObject socketObject;
            socketObject.insert(QStringLiteral("cardIndex"), socket.cardIndex);
            socketObject.insert(QStringLiteral("maxDrainPackets"),
                                static_cast<double>(socket.maxDrainPackets));
            socketStats.append(socketObject);
        }
        group.insert(QStringLiteral("sockets"), socketStats);
        receiverGroups.append(group);
    }
    snapshot.insert(QStringLiteral("receiverGroups"), receiverGroups);
    recorder->recordNetworkSnapshot(snapshot);
}

NetworkController::~NetworkController() {
    if(m_paimageTimer)m_paimageTimer->stop();
    if(m_stopThread.joinable())m_stopThread.join();
    if(m_paimage){m_paimage->stop();m_paimage.reset();}
    if(m_paimageTrace){m_paimageTrace->stop();m_paimageTrace.reset();}
    // 析构时需要同步等待后台停止线程（不能留 this 指针悬空）
    if (m_running) {
        // 析构路径不会经过 stop()，因此这里也要先留存一次接近关闭时刻的
        // 逐卡运行快照，确保窗口关闭/异常退出不会丢掉最后一段统计。
        updateAllStats();
        recordIngressSnapshot(QStringLiteral("destructor"));
        recordCardSnapshots(QStringLiteral("idle"));
        // 直接同步停止所有子线程（析构路径允许短暂阻塞）
        m_running = false;
        if (m_statsTimer) { m_statsTimer->stop(); delete m_statsTimer; m_statsTimer = nullptr; }
        for (auto& r : m_receivers) r->requestStop();
        for (auto& p : m_processors) p->requestStop();
        for (auto& s : m_savers) s->requestStop();
        if (m_publisher) m_publisher->requestInterruption();
        for (auto& r : m_receivers) {
            if (!r->wait(2000)) {
                r->forceCloseSockets();   // 先关闭 socket 再等待，避免 terminate 泄漏端口
                r->wait();   // 阻塞等待线程结束，避免析构运行中的 QThread
            }
        }
        for (auto& p : m_processors) { if (!p->wait(500)) { p->terminate(); p->wait(); } }
        for (auto& s : m_savers) { if (!s->wait(500)) { s->terminate(); s->wait(); } }
        if (m_publisher) { if (!m_publisher->wait(500)) { m_publisher->terminate(); m_publisher->wait(); } }
    }
    // 停止反馈监听线程
    m_feedbackRunning = false;
    if (m_feedbackThread.joinable()) {
        // 给 socket 发送一个空包或关闭 socket 让 recvfrom 返回
        cleanupFeedbackListener();
        m_feedbackThread.join();
    }

    // 等待后台停止线程完成（不能 detach，那样 this 会悬空）
    if (m_stopThread.joinable()) m_stopThread.join();
    cleanupControlSocket();
}

// ─────────────────────────────────────────────────────────────────────
// start()  根据 AcqConfig 创建全部线程
// ─────────────────────────────────────────────────────────────────────
bool NetworkController::start(const AcqConfig& config, std::function<void()> onStarted, std::function<void()> onFailed) {
    return startPaimage(config,std::move(onStarted),std::move(onFailed));
}

void NetworkController::rollbackStart()
{
    recordCardSnapshots(QStringLiteral("failed"));
    scheduleNetworkSnapshot(QStringLiteral("start_failure"), QStringLiteral("failed"),
                            m_currentConfigId);
    if (DiagnosticRecorder *recorder = diagnosticRecorder())
        recorder->requestFlush();

    m_running = false;
    if (m_statsTimer) { m_statsTimer->stop(); delete m_statsTimer; m_statsTimer = nullptr; }
    m_feedbackRunning = false;
    cleanupFeedbackListener();
    if (m_feedbackThread.joinable()) m_feedbackThread.join();
    if (m_retryTimer) { m_retryTimer->stop(); delete m_retryTimer; m_retryTimer = nullptr; }
    if (m_probeTimer) { m_probeTimer->stop(); delete m_probeTimer; m_probeTimer = nullptr; }
    if (m_configTimer) { m_configTimer->stop(); delete m_configTimer; m_configTimer = nullptr; }
    for (auto& r : m_receivers) r->requestStop();
    for (auto& p : m_processors) p->requestStop();
    for (auto& s : m_savers) s->requestStop();
    if (m_publisher) m_publisher->requestInterruption();
    for (auto& r : m_receivers) {
        if (!r->wait(2000)) {
            r->forceCloseSockets();   // 先关闭 socket 再等待，避免 terminate 泄漏端口
            r->wait();   // 阻塞等待线程结束，避免析构运行中的 QThread
        }
    }
    for (auto& p : m_processors) { if (!p->wait(1000)) { p->terminate(); p->wait(); } }
    for (auto& s : m_savers) { if (!s->wait(1000)) { s->terminate(); s->wait(); } }
    if (m_publisher) { if (!m_publisher->wait(1000)) { m_publisher->terminate(); m_publisher->wait(); } }
    m_receivers.clear();
    m_processors.clear();
    m_savers.clear();
    m_displayBuffers.clear();
    m_publisher.reset();
    m_cmdQueue.clear();
    clearPendingMeasurementRequests(QStringLiteral("listener_stop"));
    m_cardsReady.assign(m_config.nCards, false);
    m_configPhase = ConfigPhase::Idle;
    m_configAck.clear();
    m_configRetry.clear();
    m_configSentMs.clear();
    m_probeRetryCount = 0;
    cleanupControlSocket();
}

// ─────────────────────────────────────────────────────────────────────
// stop()  有序停止
// ─────────────────────────────────────────────────────────────────────
void NetworkController::stop() {
    if(m_paimage){stopPaimage();return;}
    // 防止重复调用
    if (!m_running && !m_stopThread.joinable()) {
        QMetaObject::invokeMethod(this, [this]() { emit stopped(); }, Qt::QueuedConnection);
        return;
    }
    if (!m_running && m_stopThread.joinable()) return;  // 后台停止线程已在运行

    m_running = false;

    if (m_statsTimer) {
        m_statsTimer->stop();
        m_statsTimer->deleteLater();
        m_statsTimer = nullptr;
    }

    // 停止反馈监听
    m_feedbackRunning = false;
    cleanupFeedbackListener();
    if (m_feedbackThread.joinable()) m_feedbackThread.join();

    // 清除命令重试 + 主动探测 + 配置确认
    if (m_retryTimer) { m_retryTimer->stop(); m_retryTimer->deleteLater(); m_retryTimer = nullptr; }
    if (m_probeTimer) { m_probeTimer->stop(); m_probeTimer->deleteLater(); m_probeTimer = nullptr; }
    if (m_configTimer) { m_configTimer->stop(); m_configTimer->deleteLater(); m_configTimer = nullptr; }
    m_probeRetryCount = 0;
    m_cmdQueue.clear();
    clearPendingMeasurementRequests(QStringLiteral("listener_stop"));
    m_cardsReady.assign(m_config.nCards, false);
    m_configPhase = ConfigPhase::Idle;
    m_configAck.clear();
    m_configRetry.clear();
    m_configSentMs.clear();
    // 停止前先刷新一次差分统计和队列深度，保证导出时至少有接近停止时刻的
    // 最终逐卡运行快照；计数器本身仍由各线程原子维护。
    updateAllStats();
    recordIngressSnapshot(QStringLiteral("stop"));
    recordCardSnapshots(QStringLiteral("idle"));

    // 发出所有停止信号（非阻塞，立即返回）
    for (auto& r : m_receivers) r->requestStop();
    for (auto& p : m_processors) p->requestStop();
    for (auto& s : m_savers) s->requestStop();
    if (m_publisher) m_publisher->requestInterruption();

    // 把容器所有权移入后台线程，避免主线程析构时阻塞
    using RecvVec  = std::vector<std::unique_ptr<MultiPortReceiver>>;
    using ProcVec  = std::vector<std::unique_ptr<DataProcessor>>;
    using SaveVec  = std::vector<std::unique_ptr<FileSaver>>;
    using PubPtr   = std::unique_ptr<FramePublisher>;
    using DispVec  = std::vector<std::unique_ptr<DisplayBuffer>>;

    auto recvMoved = std::make_shared<RecvVec>(std::move(m_receivers));
    auto procMoved = std::make_shared<ProcVec>(std::move(m_processors));
    auto saveMoved = std::make_shared<SaveVec>(std::move(m_savers));
    auto pubMoved  = std::make_shared<PubPtr>(std::move(m_publisher));
    auto dispMoved = std::make_shared<DispVec>(std::move(m_displayBuffers));

    if (m_stopThread.joinable()) m_stopThread.detach();

    m_stopThread = std::thread([this, recvMoved, procMoved, saveMoved, pubMoved, dispMoved]() {
        constexpr int WAIT_MS = 500;
        constexpr int RECV_WAIT_MS = 2000;
        for (auto& r : *recvMoved) {
            if (!r->wait(RECV_WAIT_MS)) {
                r->forceCloseSockets();   // 先关闭 socket 再等待，避免 terminate 泄漏端口
                r->wait();   // 阻塞等待线程结束，避免析构运行中的 QThread
            }
        }
        recvMoved->clear();


        for (auto& p : *procMoved) { if (!p->wait(WAIT_MS)) { p->terminate(); p->wait(); } }
        procMoved->clear();
        for (auto& s : *saveMoved) { if (!s->wait(WAIT_MS)) { s->terminate(); s->wait(); } }
        saveMoved->clear();
        if (*pubMoved) { if (!(*pubMoved)->wait(WAIT_MS)) { (*pubMoved)->terminate(); (*pubMoved)->wait(); } pubMoved->reset(); }
        dispMoved->clear();
        QMetaObject::invokeMethod(this, [this]() {
            emit statusMessage("NetworkController: 已完全停止");
            emit stopped();
        }, Qt::QueuedConnection);
    });
}

// ─
// 存储控制
// ─
void NetworkController::startSaving(const QString& directory,
                                     int triggersPerFile,
                                     const QString& suffix) {
    if(m_paimage){m_paimageSaveDir=directory;m_paimageSaveCount=triggersPerFile;m_paimageSaveSuffix=suffix;
        m_paimageSavingRequested=true;
        const auto generation=m_paimage->output().startSaving(directory,triggersPerFile,suffix);
        m_paimageSaveGeneration=generation;m_paimageSaveAppliedLogged=false;
        recordDiagnosticEvent("paimage.save","save_start_requested",DiagnosticRecorder::Severity::Info,
            {{"directory",directory},{"triggersPerFile",triggersPerFile},{"suffix",suffix},
             {"generation",QString::number(generation)},
             {"requestedMonotonicNs",QString::number(paimage::SocketReceiver::now())}});
        return;}
    for (auto& p : m_processors) p->setSaveEnabled(true);
    for (auto& s : m_savers)
        s->startSaving(directory, triggersPerFile, suffix);
}

void NetworkController::stopSaving() {
    if(m_paimage){m_paimageSavingRequested=false;
        const auto generation=m_paimage->output().stopSaving();
        recordDiagnosticEvent("paimage.save","save_stop_requested",DiagnosticRecorder::Severity::Info,
            {{"generation",QString::number(generation)},
             {"requestedMonotonicNs",QString::number(paimage::SocketReceiver::now())},
             {"directory",m_paimageSaveDir}});
        return;}
    for (auto& p : m_processors) p->setSaveEnabled(false);
    for (auto& s : m_savers) s->stopSaving();
}

// ─
// 自动保存会话代协调器（物理边界权威）
// ─
void NetworkController::configureAutoSave(const QString& baseDirectory,
                                           std::uint64_t lastDirectoryNumber)
{
    m_autoSaveCoordinator.configure(baseDirectory, lastDirectoryNumber);
}

NetworkController::AutoSaveCommit NetworkController::beginAutoSaveSession(
    std::uint64_t measurementSession, const QString& phase)
{
    auto result = m_autoSaveCoordinator.beginSession(measurementSession, phase);
    if (result.committed)
        requestCloseSavers();
    return result;
}

NetworkController::AutoSaveCommit NetworkController::commitAutoSaveBoundary(
    std::uint64_t measurementSession,
    std::uint64_t roundGeneration,
    paimage::AutoSaveBoundaryKind boundaryKind,
    const QString& phase)
{
    auto result = m_autoSaveCoordinator.commitBoundary(
        measurementSession, roundGeneration, boundaryKind, phase);
    if (result.committed)
        requestCloseSavers();
    return result;
}

void NetworkController::disableAutoSave()
{
    m_autoSaveCoordinator.disable();
}

bool NetworkController::autoSaveEnabled() const
{
    return m_autoSaveCoordinator.enabled();
}

bool NetworkController::autoSaveFaulted() const
{
    return m_autoSaveCoordinator.faulted();
}

uint64_t NetworkController::autoSessionGen() const
{
    return m_autoSaveCoordinator.currentGeneration();
}

QString NetworkController::sessionDir(uint64_t gen) const
{
    return m_autoSaveCoordinator.directoryFor(gen);
}

void NetworkController::requestCloseSavers() {
    if (m_paimage) {
        m_paimage->output().requestClose();
        return;
    }
    for (auto& s : m_savers) s->requestClose();
}

void NetworkController::setDisplayPoints(int displayPoints) {
    if (displayPoints <= 0) return;
    m_config.displayPoints = displayPoints;
    for (auto& p : m_processors) p->setDisplayPoints(displayPoints);
}

void NetworkController::setLogicalTriggersPerRound(std::uint64_t count) {
    if (count == 0 || count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        return;
    m_config.logicalTriggersPerRound = static_cast<int>(count);
    if (m_paimage)
        m_paimage->output().setConfiguredLogicalTriggersPerRound(count);
}

void NetworkController::setPhysicalRoundTimeout(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0)
        return;
    m_physicalRoundTimeoutSec = seconds;
    if (m_paimage)
        m_paimage->output().setPhysicalRoundTimeout(seconds);
}

paimage::PhysicalRoundNormalizer::Snapshot NetworkController::physicalRoundSnapshot() const {
    if (m_paimage)
        return m_paimage->output().normalizerSnapshot();
    paimage::PhysicalRoundNormalizer::Snapshot result;
    result.firstVisibleFilterMode = false;
    return result;
}

void NetworkController::reconfigure(const AcqConfig& config) {
    if(m_paimage){
        m_config.displayPoints=config.displayPoints;
        setDisplayPoints(config.displayPoints);
        if (config.logicalTriggersPerRound > 0)
            setLogicalTriggersPerRound(static_cast<std::uint64_t>(config.logicalTriggersPerRound));
        return;
    }
    m_config = config;
    for (auto& p : m_processors) p->updateConfig(config);
}

bool NetworkController::isSaving() const {
    return !m_savers.empty() && m_savers[0]->isSaving();
}

// ─
// 状态查询
// ─
std::optional<CardStats::Snapshot>
NetworkController::getCardStats(int cardIdx) const {
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_processors.size()))
        return std::nullopt;
    auto snap = m_processors[cardIdx]->statsSnapshot();
    if (cardIdx < static_cast<int>(m_savers.size()))
        snap.saveQueueDepth = m_paimage ? int(m_paimage->output().cardDepth(cardIdx)) : m_savers[cardIdx]->queueDepth();
    return snap;
}

std::vector<CardStats::Snapshot> NetworkController::getAllCardStats() const {
    std::vector<CardStats::Snapshot> result;
    for (int i = 0; i < static_cast<int>(m_processors.size()); ++i) {
        auto snap = m_processors[i]->statsSnapshot();
        if (i < static_cast<int>(m_savers.size()))
            snap.saveQueueDepth = m_paimage ? int(m_paimage->output().cardDepth(int(i))) : m_savers[i]->queueDepth();
        result.push_back(snap);
    }
    return result;
}

DisplayBuffer* NetworkController::displayBuffer(int cardIdx) const {
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_displayBuffers.size()))
        return nullptr;
    return m_displayBuffers[cardIdx].get();
}

// 
// 统计定时器（差分法计算速率，1Hz）
// 
void NetworkController::onStatsTimer() {
    updateAllStats();
}

void NetworkController::updateAllStats() {
    uint64_t now    = nowMs();
    double   dt     = static_cast<double>(now - m_lastStatsMs) / 1000.0;
    if (dt < 0.1) return;
    m_lastStatsMs = now;

    for (int i = 0; i < static_cast<int>(m_processors.size()); ++i) {
        auto& proc  = *m_processors[i];
        auto& stats = proc.stats();

        uint64_t curPkts = stats.packetsReceived.load(std::memory_order_relaxed);
        uint64_t curDrop = stats.packetsDropped.load(std::memory_order_relaxed);
        uint64_t curTrig = stats.triggersComplete.load(std::memory_order_relaxed);

        double dpkts = static_cast<double>(curPkts - m_lastPktsReceived[i]);
        double ddrop = static_cast<double>(curDrop - m_lastPktsDropped[i]);
        double dtrig = static_cast<double>(curTrig - m_lastTrigsComplete[i]);

        m_lastPktsReceived[i]  = curPkts;
        m_lastPktsDropped[i]   = curDrop;
        m_lastTrigsComplete[i] = curTrig;

        // Mbps = 包数  包大小(bit) / 时间(s) / 1e6
        stats.recvMbps       = dpkts * UDP_TOTAL_BYTES * 8.0 / dt / 1e6;
        if(m_paimage){const auto bytes=stats.socketBytesReceived.load();
            stats.recvMbps=double(bytes-m_paimageLastBytes[i])*8.0/dt/1e6;m_paimageLastBytes[i]=bytes;}
        stats.triggerHz      = dtrig / dt;
        // 丢包率（基于接收包和丢包的比例）
        double totalPkts = dpkts + ddrop;
        stats.packetLossRate = (totalPkts > 0) ? ddrop / totalPkts : 0.0;
        stats.lastUpdateMs   = m_lastStatsMs;
        // 队列深度（供 UI 监控积压情况）
        stats.inputQueueDepth = proc.inputQueueDepth();
    }

    // 运行期间每约 2 秒写入一次逐卡采集快照，避免高频刷爆诊断记录器；
    // stop() 还会额外写入一次最终快照。
    if (m_lastRuntimeSnapshotMs == 0 ||
        now - m_lastRuntimeSnapshotMs >= 2000) {
        m_lastRuntimeSnapshotMs = now;
        if(m_paimage)recordPaimageSnapshot();
        recordCardSnapshots(QStringLiteral("runtime"));
    }
    if (m_lastIngressSnapshotMs == 0 ||
        now - m_lastIngressSnapshotMs >= 2000) {
        m_lastIngressSnapshotMs = now;
        recordIngressSnapshot(QStringLiteral("runtime"));
    }
}

// =====================================================================
// UDP 控制命令实现（协议与 MC_410T_Qt 完全一致）
// =====================================================================

bool NetworkController::initControlSocket()
{
    cleanupControlSocket();
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    m_controlSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_controlSocket == INVALID_SOCKET) {
        emit errorOccurred("创建控制 socket 失败");
        QJsonObject fields;
        fields.insert(QStringLiteral("socket"), QStringLiteral("control"));
#ifdef _WIN32
        fields.insert(QStringLiteral("errorCode"), WSAGetLastError());
#endif
        recordDiagnosticEvent(QStringLiteral("network.control"),
                              QStringLiteral("socket_create_failed"),
                              DiagnosticRecorder::Severity::Error,
                              fields);
        return false;
    }

    // ── 绑定到指定本地IP，确保控制包从正确网络接口发出 ──────────────────
    // 双口网卡（如 ConnectX-5 MCX512A-ACAT）只接一个口时，必须绑定到
    // 已连接口的IP，否则 Windows 路由可能将数据包从另一个口（断路）发出，
    // 导致采集卡收不到控制命令，Wireshark 也抓不到任何控制包。
    // 配置方式：注册表 HKCU\Software\MC410T\MC410T_Receiver\NetworkParams\LocalBindIP
    if (!m_config.localBindIP.empty()) {
        sockaddr_in localAddr;
        memset(&localAddr, 0, sizeof(localAddr));
        localAddr.sin_family      = AF_INET;
        localAddr.sin_port        = 0;   // 本地端口任意
        inet_pton(AF_INET, m_config.localBindIP.c_str(), &localAddr.sin_addr);
        if (::bind(m_controlSocket,
                   reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) != 0) {
#ifdef _WIN32
            const int errorCode = WSAGetLastError();
            emit errorOccurred(
                QString("控制 socket 绑定本地IP %1 失败（WSA=%2），将使用默认路由，控制包可能从错误接口发出")
                .arg(QString::fromStdString(m_config.localBindIP)).arg(errorCode));
            QJsonObject fields;
            fields.insert(QStringLiteral("socket"), QStringLiteral("control"));
            fields.insert(QStringLiteral("localAddress"), QString::fromStdString(m_config.localBindIP));
            fields.insert(QStringLiteral("errorCode"), errorCode);
            recordDiagnosticEvent(QStringLiteral("network.control"),
                                  QStringLiteral("bind_failed"),
                                  DiagnosticRecorder::Severity::Error,
                                  fields);
#else
            emit errorOccurred(
                QString("控制 socket 绑定本地IP %1 失败，将使用默认路由")
                .arg(QString::fromStdString(m_config.localBindIP)));
            QJsonObject fields;
            fields.insert(QStringLiteral("socket"), QStringLiteral("control"));
            fields.insert(QStringLiteral("localAddress"), QString::fromStdString(m_config.localBindIP));
            recordDiagnosticEvent(QStringLiteral("network.control"),
                                  QStringLiteral("bind_failed"),
                                  DiagnosticRecorder::Severity::Error,
                                  fields);
#endif
        } else {
            emit statusMessage(
                QString("控制 socket 已绑定到本地接口 %1，控制包将从该接口发出")
                .arg(QString::fromStdString(m_config.localBindIP)));
            QJsonObject fields;
            fields.insert(QStringLiteral("socket"), QStringLiteral("control"));
            fields.insert(QStringLiteral("localAddress"), QString::fromStdString(m_config.localBindIP));
            fields.insert(QStringLiteral("bindResult"), QStringLiteral("ok"));
            recordDiagnosticEvent(QStringLiteral("network.control"),
                                  QStringLiteral("bind_succeeded"),
                                  DiagnosticRecorder::Severity::Info,
                                  fields);
        }
    } else {
        emit statusMessage("控制 socket 使用默认路由（提示：双口网卡建议在注册表 NetworkParams/LocalBindIP 中指定本地IP）");
        QJsonObject fields;
        fields.insert(QStringLiteral("socket"), QStringLiteral("control"));
        fields.insert(QStringLiteral("bindResult"), QStringLiteral("default_route"));
        recordDiagnosticEvent(QStringLiteral("network.control"),
                              QStringLiteral("bind_default_route"),
                              DiagnosticRecorder::Severity::Info,
                              fields);
    }
    return true;
}

void NetworkController::cleanupControlSocket()
{
    if (m_controlSocket != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(m_controlSocket);
        WSACleanup();
#else
        close(m_controlSocket);
#endif
        m_controlSocket = INVALID_SOCKET;
    }
}

// ═════════════════════════════════════════════════════════════════════
// 反馈监听（端口 8000）：检测采集卡发送的 18 字节就绪包
//
// 采集卡上电后 FPGA 网络栈初始化需要 ~130 秒，完成后每张卡会向
// 主机 IP:8000 发送一个 18 字节的 UDP 包作为"就绪"信号。
// 只有收到此包后，该卡才能正常接收配置/测量命令。
// ═════════════════════════════════════════════════════════════════════

bool NetworkController::initFeedbackListener()
{
    cleanupFeedbackListener();

    m_feedbackSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_feedbackSocket == INVALID_SOCKET) {
        emit errorOccurred("创建反馈监听 socket 失败");
        QJsonObject fields;
        fields.insert(QStringLiteral("socket"), QStringLiteral("feedback"));
#ifdef _WIN32
        fields.insert(QStringLiteral("errorCode"), WSAGetLastError());
#endif
        recordDiagnosticEvent(QStringLiteral("network.feedback"),
                              QStringLiteral("socket_create_failed"),
                              DiagnosticRecorder::Severity::Error,
                              fields);
        return false;
    }

    // 设置接收超时（1秒），使线程能响应退出请求
#ifdef _WIN32
    DWORD timeout = 1000;
    setsockopt(m_feedbackSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(m_feedbackSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    // 绑定到 FEEDBACK_PORT（8000），监听所有接口
    sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_port   = htons(FEEDBACK_PORT);
    localAddr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(m_feedbackSocket,
               reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) != 0) {
#ifdef _WIN32
        const int errorCode = WSAGetLastError();
        emit errorOccurred(QString("反馈 socket 绑定端口 %1 失败（WSA=%2），将无法检测卡片就绪状态")
                          .arg(FEEDBACK_PORT).arg(errorCode));
        QJsonObject fields;
        fields.insert(QStringLiteral("socket"), QStringLiteral("feedback"));
        fields.insert(QStringLiteral("port"), FEEDBACK_PORT);
        fields.insert(QStringLiteral("errorCode"), errorCode);
        recordDiagnosticEvent(QStringLiteral("network.feedback"),
                              QStringLiteral("bind_failed"),
                              DiagnosticRecorder::Severity::Error,
                              fields);
#else
        emit errorOccurred(QString("反馈 socket 绑定端口 %1 失败，将无法检测卡片就绪状态")
                          .arg(FEEDBACK_PORT));
        QJsonObject fields;
        fields.insert(QStringLiteral("socket"), QStringLiteral("feedback"));
        fields.insert(QStringLiteral("port"), FEEDBACK_PORT);
        recordDiagnosticEvent(QStringLiteral("network.feedback"),
                              QStringLiteral("bind_failed"),
                              DiagnosticRecorder::Severity::Error,
                              fields);
#endif
        cleanupFeedbackListener();
        return false;
    }

    m_feedbackRunning = true;
    m_feedbackThread = std::thread(&NetworkController::feedbackListenerThread, this);

    // 创建重试定时器（500ms 间隔，用于重新发送等待就绪的命令）
    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setInterval(500);
        connect(m_retryTimer, &QTimer::timeout, this, &NetworkController::retryPendingCommand);
    }

    // 启动主动探测定时器（8 秒后对未就绪卡进行 ARP 探测，
    // 解决上位机重启后漏掉 18 字节就绪包的问题）
    startProbeTimer();

    emit statusMessage(QString("反馈监听已启动（端口 %1），等待卡片就绪信号...").arg(FEEDBACK_PORT));
    QJsonObject fields;
    fields.insert(QStringLiteral("socket"), QStringLiteral("feedback"));
    fields.insert(QStringLiteral("port"), FEEDBACK_PORT);
    fields.insert(QStringLiteral("bindResult"), QStringLiteral("ok"));
    recordDiagnosticEvent(QStringLiteral("network.feedback"),
                          QStringLiteral("listener_started"),
                          DiagnosticRecorder::Severity::Info,
                          fields);
    return true;
}

void NetworkController::cleanupFeedbackListener()
{
    if (m_feedbackSocket != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(m_feedbackSocket);
#else
        close(m_feedbackSocket);
#endif
        m_feedbackSocket = INVALID_SOCKET;
    }
}

void NetworkController::feedbackListenerThread()
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    char buf[64];
    sockaddr_in senderAddr;

    emit statusMessage("[反馈监听] 线程已启动");

    while (m_feedbackRunning) {
        socklen_t senderLen = sizeof(senderAddr);
        int n = recvfrom(m_feedbackSocket, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);
        if (n <= 0) {
            if (!m_feedbackRunning) break;

            int errorCode = 0;
#ifdef _WIN32
            errorCode = WSAGetLastError();
            const bool timeout = errorCode == WSAETIMEDOUT;
#else
            errorCode = errno;
            const bool timeout = errorCode == EAGAIN || errorCode == EWOULDBLOCK;
#endif
            if (timeout) {
                const quint64 timeoutCount =
                    m_feedbackTimeoutCount.fetch_add(1, std::memory_order_relaxed) + 1;
                // SO_RCVTIMEO 是正常的线程退出轮询；只记录首个及每 60 次，
                // 避免空闲链路把诊断队列刷满。
                if (timeoutCount == 1 || timeoutCount % 60 == 0) {
                    if (DiagnosticRecorder *recorder = diagnosticRecorder()) {
                        QJsonObject fields;
                        fields.insert(QStringLiteral("errorCode"), errorCode);
                        fields.insert(QStringLiteral("timeout"), true);
                        fields.insert(QStringLiteral("timeoutCount"),
                                      static_cast<double>(timeoutCount));
                        recorder->recordEvent(QStringLiteral("network.feedback"),
                                              QStringLiteral("recv_timeout"),
                                              DiagnosticRecorder::Severity::Debug,
                                              fields);
                    }
                }
            } else if (DiagnosticRecorder *recorder = diagnosticRecorder()) {
                QJsonObject fields;
                fields.insert(QStringLiteral("errorCode"), errorCode);
                fields.insert(QStringLiteral("timeout"), false);
                recorder->recordEvent(QStringLiteral("network.feedback"),
                                      QStringLiteral("recv_error"),
                                      DiagnosticRecorder::Severity::Warning,
                                      fields);
            }
            continue;
        }

        char srcIP[64] = {};
        inet_ntop(AF_INET, &senderAddr.sin_addr, srcIP, sizeof(srcIP));
        const QString ipStr = QString::fromLatin1(srcIP);
        const quint16 sourcePort = ntohs(senderAddr.sin_port);
        const quint64 receiveSequence =
            m_feedbackSequence.fetch_add(1, std::memory_order_relaxed) + 1;

        if (DiagnosticRecorder *recorder = diagnosticRecorder()) {
            QJsonObject fields;
            fields.insert(QStringLiteral("receiveSequence"),
                          static_cast<double>(receiveSequence));
            fields.insert(QStringLiteral("sourceIP"), ipStr);
            fields.insert(QStringLiteral("sourcePort"), sourcePort);
            fields.insert(QStringLiteral("length"), n);
            fields.insert(QStringLiteral("hex"),
                          NetworkDiagnostics::formatHex(QByteArray(buf, n)));
            fields.insert(QStringLiteral("packetType"),
                          n == 18 ? QStringLiteral("ready18")
                                  : (n == 60 ? QStringLiteral("configAck60")
                                             : QStringLiteral("unknown")));
            recorder->recordEvent(QStringLiteral("network.feedback"),
                                  QStringLiteral("packet_received"),
                                  DiagnosticRecorder::Severity::Info,
                                  fields);
        }

        // 将发送方 IP 映射到卡索引（18 字节就绪包 / 60 字节配置反馈共用）
        int cardIdx = -1;
        for (int i = 0; i < m_targetIPs.size(); ++i) {
            if (m_targetIPs[i] == ipStr) {
                cardIdx = i;
                break;
            }
        }

        if (n == 18) {
            // ── 18 字节：卡版本号/就绪包 ─────────────────────────
            // 上电后主动上报；也可能在收到配置指令后立即回发。
            // 注意：这不代表配置成功，WaitingAck 时需重发配置。
            if (cardIdx < 0) {
                emit statusMessage(QString("[反馈监听] 收到未知来源的 18 字节包: %1（非目标卡IP）").arg(ipStr));
                if (DiagnosticRecorder *recorder = diagnosticRecorder()) {
                    QJsonObject fields;
                    fields.insert(QStringLiteral("receiveSequence"),
                                  static_cast<double>(receiveSequence));
                    fields.insert(QStringLiteral("sourceIP"), ipStr);
                    fields.insert(QStringLiteral("sourcePort"), sourcePort);
                    fields.insert(QStringLiteral("reason"), QStringLiteral("unknownSource"));
                    recorder->recordEvent(QStringLiteral("network.feedback"),
                                          QStringLiteral("ready_packet_rejected"),
                                          DiagnosticRecorder::Severity::Warning,
                                          fields);
                }
                continue;
            }
            QMetaObject::invokeMethod(this, [this, cardIdx, receiveSequence]() {
                onReadyPacket(cardIdx, receiveSequence);
            }, Qt::QueuedConnection);
        } else if (n == 60) {
            // ── 60 字节：配置参数反馈 ─────────────────────────────
            // 仅配置参数指令有此反馈，表示采集卡已收到配置。
            // 反馈按单卡独立上报（以来源 IP 区分）。
            if (cardIdx < 0) {
                emit statusMessage(QString("[反馈监听] 收到未知来源的 60 字节反馈: %1（非目标卡IP）").arg(ipStr));
                if (DiagnosticRecorder *recorder = diagnosticRecorder()) {
                    QJsonObject fields;
                    fields.insert(QStringLiteral("receiveSequence"),
                                  static_cast<double>(receiveSequence));
                    fields.insert(QStringLiteral("sourceIP"), ipStr);
                    fields.insert(QStringLiteral("sourcePort"), sourcePort);
                    fields.insert(QStringLiteral("reason"), QStringLiteral("unknownSource"));
                    recorder->recordEvent(QStringLiteral("network.feedback"),
                                          QStringLiteral("ack_packet_rejected"),
                                          DiagnosticRecorder::Severity::Warning,
                                          fields);
                }
                continue;
            }
            QMetaObject::invokeMethod(this, [this, cardIdx, receiveSequence]() {
                onConfigAck(cardIdx, receiveSequence);
            }, Qt::QueuedConnection);
        } else {
            emit statusMessage(QString("[反馈监听] 忽略未知包: %1 字节来自 %2")
                               .arg(n)
                               .arg(ipStr));
            if (DiagnosticRecorder *recorder = diagnosticRecorder()) {
                QJsonObject fields;
                fields.insert(QStringLiteral("receiveSequence"),
                              static_cast<double>(receiveSequence));
                fields.insert(QStringLiteral("sourceIP"), ipStr);
                fields.insert(QStringLiteral("sourcePort"), sourcePort);
                fields.insert(QStringLiteral("reason"), QStringLiteral("unknownLength"));
                recorder->recordEvent(QStringLiteral("network.feedback"),
                                      QStringLiteral("packet_rejected"),
                                      DiagnosticRecorder::Severity::Warning,
                                      fields);
            }
            continue;
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif
    emit statusMessage("[反馈监听] 线程已退出");
}

void NetworkController::markCardReady(int cardIdx, bool viaReadyPacket)
{
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_cardsReady.size()))
        return;
    if (m_cardsReady[cardIdx])
        return;  // 已标记，忽略重复

    m_cardsReady[cardIdx] = true;
    QJsonObject fields;
    fields.insert(QStringLiteral("cardIndex"), cardIdx + 1);
    fields.insert(QStringLiteral("ip"), m_targetIPs.value(cardIdx));
    fields.insert(QStringLiteral("viaReadyPacket"), viaReadyPacket);
    recordDiagnosticEvent(QStringLiteral("network.ready"),
                          QStringLiteral("card_marked_ready"),
                          DiagnosticRecorder::Severity::Info,
                          fields);
    if (viaReadyPacket)
        emit statusMessage(QString("卡%1（%2）FPGA 就绪 ✓（收到 18 字节就绪包）")
                          .arg(cardIdx + 1).arg(m_targetIPs[cardIdx]));
    else
        emit statusMessage(QString("卡%1（%2）标记为就绪 ✓（ARP 探测）")
                          .arg(cardIdx + 1).arg(m_targetIPs[cardIdx]));
    emit cardReady(cardIdx);
    recordCardSnapshots();

    // 检查是否所有卡均已就绪
    if (isAllCardsReady()) {
        emit statusMessage("所有采集卡均已就绪");
        emit allCardsReady();

        // 如果有等待的命令，立即执行（FPGA 现在能正确处理了）
        if (!m_cmdQueue.empty()) {
            emit statusMessage("FPGA 就绪，开始执行等待中的命令...");
            retryPendingCommand();
        }
    } else {
        // 部分就绪时，如果有等待命令且刚就绪的卡是目标卡之一，也尝试重试
        if (!m_cmdQueue.empty()) {
            retryPendingCommand();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════
// 配置确认状态机（60 字节反馈）
//
// 背景：配置参数指令下发后，必须收到采集卡反馈的 60 字节 UDP 包才能
//       进行测量控制；否则采集卡内部会禁止执行开始测量指令。
//       反馈按单卡独立返回，重发仅针对未确认的卡。
// 特殊：采集卡上电后立即下发配置，会先收到 18 字节版本号包，
//       这不代表配置成功，必须重发配置直到收到 60 字节反馈。
// ═════════════════════════════════════════════════════════════════════

void NetworkController::onReadyPacket(int cardIdx, quint64 receiveSequence)
{
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_cardsReady.size())) {
        QJsonObject fields;
        fields.insert(QStringLiteral("receiveSequence"),
                      static_cast<double>(receiveSequence));
        fields.insert(QStringLiteral("cardIndex"), cardIdx + 1);
        fields.insert(QStringLiteral("reason"), QStringLiteral("unknownCard"));
        recordDiagnosticEvent(QStringLiteral("network.ready"),
                              QStringLiteral("ready_callback_rejected"),
                              DiagnosticRecorder::Severity::Warning,
                              fields);
        return;
    }

    if (cardIdx < static_cast<int>(m_cardDiagnostics.size()))
        ++m_cardDiagnostics[cardIdx].readyPacketCount;

    const bool duplicate = m_cardsReady[cardIdx];
    QJsonObject receivedFields;
    receivedFields.insert(QStringLiteral("receiveSequence"),
                          static_cast<double>(receiveSequence));
    receivedFields.insert(QStringLiteral("cardIndex"), cardIdx + 1);
    receivedFields.insert(QStringLiteral("configId"), m_currentConfigId);
    receivedFields.insert(QStringLiteral("duplicate"), duplicate);
    recordDiagnosticEvent(QStringLiteral("network.ready"),
                          duplicate ? QStringLiteral("ready_packet_duplicate")
                                    : QStringLiteral("ready_packet_associated"),
                          duplicate ? DiagnosticRecorder::Severity::Debug
                                    : DiagnosticRecorder::Severity::Info,
                          receivedFields);

    markCardReady(cardIdx, true);   // 真正收到 18 字节版本号/就绪包

    // 正在等待配置确认时收到 18 字节包：非配置成功反馈，需重发该卡配置
    if (m_configPhase == ConfigPhase::WaitingAck &&
        cardIdx >= 0 && cardIdx < static_cast<int>(m_configAck.size()) &&
        !m_configAck[cardIdx]) {
        emit statusMessage(QString("卡%1 收到 18 字节版本号包，重发配置参数...").arg(cardIdx + 1));
        const int previousRetry = m_configRetry[cardIdx];
        if (cardIdx < static_cast<int>(m_cardDiagnostics.size())) {
            ++m_cardDiagnostics[cardIdx].retryResetCount;
        }
        QJsonObject resetFields;
        resetFields.insert(QStringLiteral("cardIndex"), cardIdx + 1);
        resetFields.insert(QStringLiteral("receiveSequence"),
                           static_cast<double>(receiveSequence));
        resetFields.insert(QStringLiteral("configId"), m_currentConfigId);
        resetFields.insert(QStringLiteral("reason"), QStringLiteral("ready18"));
        resetFields.insert(QStringLiteral("previousRetry"), previousRetry);
        resetFields.insert(QStringLiteral("retryReset"), true);
        recordDiagnosticEvent(QStringLiteral("network.config"),
                              QStringLiteral("retry_reset"),
                              DiagnosticRecorder::Severity::Info,
                              resetFields);
        m_configRetry[cardIdx] = 0;   // 卡处于活跃响应，重置重试计数
        doSendConfigTo(cardIdx, QStringLiteral("18_retry"));
    }
}

void NetworkController::beginConfigWait(const PendingConfig& pc)
{
    m_pendingConfig = pc;
    m_currentConfigId = pc.configId;
    m_currentConfigTrigger = pc.trigger;
    m_configPhase   = ConfigPhase::WaitingAck;

    const int n = static_cast<int>(m_targetIPs.size());
    m_configAck.assign(n, false);
    m_configRetry.assign(n, 0);
    m_configSentMs.assign(n, 0);

    if (DiagnosticRecorder *recorder = diagnosticRecorder()) {
        QJsonObject settings;
        settings.insert(QStringLiteral("listenId"), m_diagnosticListenId);
        settings.insert(QStringLiteral("source"), m_diagnosticSource);
        settings.insert(QStringLiteral("configId"), m_currentConfigId);
        settings.insert(QStringLiteral("trigger"), m_currentConfigTrigger);
        settings.insert(QStringLiteral("dataTime"), pc.dataTime);
        settings.insert(QStringLiteral("aDelay"), pc.aDelay);
        settings.insert(QStringLiteral("bDelay"), pc.bDelay);
        recorder->recordSettingsSnapshot(settings);
    }
    QJsonObject waitFields;
    waitFields.insert(QStringLiteral("configId"), m_currentConfigId);
    waitFields.insert(QStringLiteral("trigger"), m_currentConfigTrigger);
    waitFields.insert(QStringLiteral("cardCount"), n);
    recordDiagnosticEvent(QStringLiteral("network.config"),
                          QStringLiteral("config_wait_started"),
                          DiagnosticRecorder::Severity::Info,
                          waitFields);
    recordCardSnapshots();

    if (!m_configTimer) {
        m_configTimer = new QTimer(this);
        m_configTimer->setInterval(CONFIG_ACK_TICK_MS);
        connect(m_configTimer, &QTimer::timeout, this, &NetworkController::onConfigTimerTick);
    }

    // 逐卡下发配置（per-card 独立确认）
    for (int i = 0; i < n; ++i)
        doSendConfigTo(i, QStringLiteral("first"));

    m_configTimer->start();
    emit statusMessage(QString("配置参数已下发（%1 张卡），等待 60 字节反馈确认...").arg(n));
}

bool NetworkController::doSendConfigTo(int cardIdx, const QString& reason)
{
    if (m_controlSocket == INVALID_SOCKET) return false;
    if (cardIdx < 0 || cardIdx >= m_targetIPs.size()) return false;
    QByteArray cmd = buildConfigPacket(m_pendingConfig.dataTime,
                                       m_pendingConfig.aDelay,
                                       m_pendingConfig.bDelay);
    bool ok = sendRawCommand(cmd, m_targetIPs[cardIdx], reason);
    if (cardIdx >= 0 && cardIdx < static_cast<int>(m_configSentMs.size()))
        m_configSentMs[cardIdx] = nowMs();
    return ok;
}

void NetworkController::onConfigAck(int cardIdx, quint64 receiveSequence)
{
    if (cardIdx >= 0 && cardIdx < static_cast<int>(m_cardDiagnostics.size()))
        ++m_cardDiagnostics[cardIdx].ackPacketCount;

    QJsonObject fields;
    fields.insert(QStringLiteral("receiveSequence"),
                  static_cast<double>(receiveSequence));
    fields.insert(QStringLiteral("cardIndex"), cardIdx + 1);
    fields.insert(QStringLiteral("configId"), m_currentConfigId);

    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_configAck.size())) {
        fields.insert(QStringLiteral("reason"), QStringLiteral("unknownCard"));
        recordDiagnosticEvent(QStringLiteral("network.config"),
                              QStringLiteral("ack_rejected"),
                              DiagnosticRecorder::Severity::Warning,
                              fields);
        return;
    }
    if (m_configPhase != ConfigPhase::WaitingAck) {
        fields.insert(QStringLiteral("phase"), cardDiagnosticState(cardIdx));
        fields.insert(QStringLiteral("reason"), QStringLiteral("nonWaiting"));
        recordDiagnosticEvent(QStringLiteral("network.config"),
                              QStringLiteral("ack_rejected"),
                              DiagnosticRecorder::Severity::Warning,
                              fields);
        return;
    }
    if (m_configAck[cardIdx]) {
        fields.insert(QStringLiteral("reason"), QStringLiteral("duplicate"));
        recordDiagnosticEvent(QStringLiteral("network.config"),
                              QStringLiteral("ack_duplicate"),
                              DiagnosticRecorder::Severity::Debug,
                              fields);
        return;  // 已确认，忽略重复
    }

    m_configAck[cardIdx] = true;
    fields.insert(QStringLiteral("reason"), QStringLiteral("accepted"));
    recordDiagnosticEvent(QStringLiteral("network.config"),
                          QStringLiteral("ack_associated"),
                          DiagnosticRecorder::Severity::Info,
                          fields);
    emit statusMessage(QString("卡%1 配置确认 ✓（收到 60 字节反馈）").arg(cardIdx + 1));
    emit configAcked(cardIdx);
    recordCardSnapshots();

    if (isAllConfigAcked()) {
        m_configPhase = ConfigPhase::Confirmed;
        if (m_configTimer) m_configTimer->stop();
        emit statusMessage("所有采集卡配置参数均已确认");
        emit configConfirmed();
        QJsonObject confirmedFields;
        confirmedFields.insert(QStringLiteral("configId"), m_currentConfigId);
        confirmedFields.insert(QStringLiteral("cardCount"), static_cast<int>(m_configAck.size()));
        recordDiagnosticEvent(QStringLiteral("network.config"),
                              QStringLiteral("config_confirmed"),
                              DiagnosticRecorder::Severity::Info,
                              confirmedFields);
        recordCardSnapshots();
        // 若有等待配置确认的测量命令 → 自动执行
        retryPendingCommand();
    }
}

void NetworkController::onConfigTimerTick()
{
    if (m_configPhase != ConfigPhase::WaitingAck) return;

    const uint64_t now = nowMs();
    bool anyFailed = false;
    for (int i = 0; i < static_cast<int>(m_configAck.size()); ++i) {
        if (m_configAck[i]) continue;   // 已确认
        if (i >= static_cast<int>(m_configSentMs.size())) continue;
        if (now - m_configSentMs[i] < CONFIG_ACK_TIMEOUT_MS) continue;  // 未到超时

        if (m_configRetry[i] >= CONFIG_ACK_MAX_RETRY) {
            // 重发超限：判定失败（多卡联动，任一失败即整体失败）
            anyFailed = true;
            emit statusMessage(QString("卡%1 配置确认失败（多次重发未收到 60 字节反馈）").arg(i + 1));
            emit configAckFailed(i);
            QJsonObject fields;
            fields.insert(QStringLiteral("cardIndex"), i + 1);
            fields.insert(QStringLiteral("configId"), m_currentConfigId);
            fields.insert(QStringLiteral("reason"), QStringLiteral("retry_exhausted"));
            fields.insert(QStringLiteral("retryCount"), m_configRetry[i]);
            recordDiagnosticEvent(QStringLiteral("network.config"),
                                  QStringLiteral("ack_failed"),
                                  DiagnosticRecorder::Severity::Error,
                                  fields);
        } else {
            ++m_configRetry[i];
            if (i < static_cast<int>(m_cardDiagnostics.size()))
                ++m_cardDiagnostics[i].retryCount;
            emit statusMessage(QString("卡%1 配置反馈超时，重发配置（第 %2/%3 次）")
                              .arg(i + 1).arg(m_configRetry[i]).arg(CONFIG_ACK_MAX_RETRY));
            doSendConfigTo(i, QStringLiteral("timeout_retry"));
        }
    }

    if (anyFailed) {
        m_configPhase = ConfigPhase::Failed;
        if (m_configTimer) m_configTimer->stop();
        emit statusMessage("配置确认失败：存在未反馈的采集卡，请检查链路后重新下发配置");
        QJsonObject fields;
        fields.insert(QStringLiteral("configId"), m_currentConfigId);
        fields.insert(QStringLiteral("phase"), QStringLiteral("failed"));
        fields.insert(QStringLiteral("reason"), QStringLiteral("card_ack_timeout"));
        recordDiagnosticEvent(QStringLiteral("network.config"),
                              QStringLiteral("config_failed"),
                              DiagnosticRecorder::Severity::Error,
                              fields);
        recordCardSnapshots();
        scheduleNetworkSnapshot(QStringLiteral("config_failure"),
                                QStringLiteral("failed"), m_currentConfigId);
        if (DiagnosticRecorder *recorder = diagnosticRecorder())
            recorder->requestFlush();
        clearPendingMeasurementRequests(QStringLiteral("config_failed"));
    }
}

bool NetworkController::isAllConfigAcked() const
{
    return !m_configAck.empty() &&
           std::all_of(m_configAck.begin(), m_configAck.end(), [](bool v) { return v; });
}

void NetworkController::retryPendingCommand()
{
    if (m_cmdQueue.empty()) {
        if (m_retryTimer) m_retryTimer->stop();
        return;
    }

    // 未全部就绪：继续等待（重试定时器已在运行）
    if (!isAllCardsReady()) {
        if (m_retryTimer && !m_retryTimer->isActive())
            m_retryTimer->start();
        return;
    }

    // 处理队首命令（按入队顺序：先配置、后测量）
    const PendingCmd front = m_cmdQueue.front();
    if (front.type == PendingCmdType::Config) {
        // 配置已在下发确认中，等待其完成（避免重复下发）
        if (m_configPhase == ConfigPhase::WaitingAck) {
            if (m_retryTimer && !m_retryTimer->isActive()) m_retryTimer->start();
            return;
        }
        if (m_retryTimer) m_retryTimer->stop();
        m_cmdQueue.pop_front();
        beginConfigWait({front.dataTime, front.aDelay, front.bDelay,
                         front.configId, front.trigger});
        return;
    }

    // 测量命令（Start / Stop）：必须在配置确认后才能执行
    if (isConfigConfirmed()) {
        if (m_retryTimer) m_retryTimer->stop();
        const bool isStart = front.type == PendingCmdType::StartMeasure;
        const QString sessionId = front.measurementSessionId.isEmpty()
            ? m_measurementSessionId : front.measurementSessionId;
        bool result = isStart
                    ? executeStartTransaction(sessionId)
                    : executeStopTransaction(sessionId);
        QJsonObject fields;
        fields.insert(QStringLiteral("command"),
                      front.type == PendingCmdType::StartMeasure
                          ? QStringLiteral("start") : QStringLiteral("stop"));
        fields.insert(QStringLiteral("configId"), m_currentConfigId);
        fields.insert(QStringLiteral("measurementSessionId"), sessionId);
        fields.insert(QStringLiteral("outcome"),
                      result ? QStringLiteral("executed") : QStringLiteral("rejected"));
        fields.insert(QStringLiteral("reason"),
                      result ? QStringLiteral("config_confirmed")
                            : QStringLiteral("send_failed"));
        recordDiagnosticEvent(QStringLiteral("network.measure"),
                              result ? QStringLiteral("command_executed")
                                     : QStringLiteral("command_rejected"),
                              result ? DiagnosticRecorder::Severity::Info
                                     : DiagnosticRecorder::Severity::Error,
                              fields);
        if (result) emit statusMessage("待执行的命令已成功发送");
        else emit errorOccurred("待执行的命令发送失败");
        m_cmdQueue.pop_front();
        retryPendingCommand();   // 继续处理队列中的后续命令
        return;
    }
    if (m_configPhase == ConfigPhase::Failed) {
        if (m_retryTimer) m_retryTimer->stop();
        emit errorOccurred("配置未确认（存在失败卡），无法执行测量命令，请重新下发配置");
        QJsonObject fields;
        fields.insert(QStringLiteral("command"),
                          front.type == PendingCmdType::StartMeasure
                              ? QStringLiteral("start") : QStringLiteral("stop"));
        fields.insert(QStringLiteral("configId"), m_currentConfigId);
        fields.insert(QStringLiteral("measurementSessionId"), front.measurementSessionId);
        fields.insert(QStringLiteral("outcome"), QStringLiteral("rejected"));
        fields.insert(QStringLiteral("reason"), QStringLiteral("config_failed"));
        recordDiagnosticEvent(QStringLiteral("network.measure"),
                              QStringLiteral("command_rejected"),
                              DiagnosticRecorder::Severity::Error,
                              fields);
        m_cmdQueue.pop_front();
        clearPendingMeasurementRequests(QStringLiteral("queued_config_failed"));
        return;
    }
    // Idle / WaitingAck：等待配置确认完成（configConfirmed 会再次触发本函数）
    if (m_retryTimer && !m_retryTimer->isActive()) m_retryTimer->start();
}

// ═════════════════════════════════════════════════════════════════════
// 主动探测（ARP 兜底）
//
// 场景 A：采集卡上电已久、已发送过 18 字节就绪包，但上位机 PC 刚刚重启，
//         此时被动监听永远等不到就绪包。
// 场景 B：冷启动时 ARP 先于 18 字节包就绪，提前标记。
//
// 机制：启动 8 秒后，对仍未就绪的卡调用 SendARP 探测其可达性。
//       若 ARP 能解析到 MAC 地址 → 卡已在线 → 标记为就绪。
//       注意：18 字节就绪包是卡冷启动时一次性发送的，后续重启上位机时
//       卡不会再发此包，因此 ARP 探测成功后必须直接标记就绪。
//       每轮探测间隔 5 秒，最多重试 6 轮（共约 38 秒）。
// ═════════════════════════════════════════════════════════════════════

void NetworkController::startProbeTimer()
{
    if (m_probeTimer) {
        m_probeTimer->stop();
        m_probeTimer->deleteLater();
    }
    m_probeRetryCount = 0;
    m_probeTimer = new QTimer(this);
    m_probeTimer->setSingleShot(true);
    connect(m_probeTimer, &QTimer::timeout, this, &NetworkController::onProbeTimeout);
    m_probeTimer->start(8000);  // 8 秒后首次探测
}

void NetworkController::onProbeTimeout()
{
    if (isAllCardsReady()) {
        // 全部已就绪，不需要探测
        return;
    }

    const int maxRetries = 6;
    if (++m_probeRetryCount > maxRetries) {
        emit statusMessage(QString("主动探测结束：仍有 %1/%2 张卡未就绪，可尝试重启采集卡")
                          .arg(m_targetIPs.size() - readyCardCount())
                          .arg(m_targetIPs.size()));
        return;
    }

    bool anyProbed = false;
    for (int i = 0; i < static_cast<int>(m_targetIPs.size()); ++i) {
        if (m_cardsReady[i]) continue;  // 已就绪，跳过

        // ── 用 SendARP 探测卡是否可达 ──────────────────────────
        QString ipStr = m_targetIPs[i];
        bool reachable = false;
        const uint64_t arpStartMs = nowMs();
        quint64 arpError = 0;
        QString arpMac = QStringLiteral("unknown");

#ifdef _WIN32
        ULONG macBuf[2] = {};      // 6 字节 MAC 用 2 个 ULONG 装
        ULONG macLen = sizeof(macBuf);
        IPAddr dest = inet_addr(ipStr.toStdString().c_str());

        DWORD arpRet = SendARP(dest, 0, macBuf, &macLen);
        arpError = arpRet;
        if (arpRet == NO_ERROR && macLen >= 6) {
            reachable = true;
            const auto *mac = static_cast<const uint8_t*>(static_cast<const void*>(macBuf));
            arpMac = QStringLiteral("%1:%2:%3:%4:%5:%6")
                .arg(mac[0], 2, 16, QLatin1Char('0'))
                .arg(mac[1], 2, 16, QLatin1Char('0'))
                .arg(mac[2], 2, 16, QLatin1Char('0'))
                .arg(mac[3], 2, 16, QLatin1Char('0'))
                .arg(mac[4], 2, 16, QLatin1Char('0'))
                .arg(mac[5], 2, 16, QLatin1Char('0')).toUpper();
            emit statusMessage(QString("  ARP 探测成功: %1 → %02X:%02X:%02X:%02X:%02X:%02X")
                              .arg(ipStr)
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[0])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[1])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[2])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[3])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[4])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[5]));
        }
#endif

        QJsonObject arpFields;
        arpFields.insert(QStringLiteral("ip"), ipStr);
        arpFields.insert(QStringLiteral("probe"), QStringLiteral("arp"));
        arpFields.insert(QStringLiteral("arpFallback"), true);
        arpFields.insert(QStringLiteral("arpResult"), reachable
                         ? QStringLiteral("reachable")
                         : QStringLiteral("unreachable"));
        arpFields.insert(QStringLiteral("mac"), arpMac);
        arpFields.insert(QStringLiteral("errorCode"), static_cast<double>(arpError));
        arpFields.insert(QStringLiteral("elapsedMs"),
                         static_cast<double>(nowMs() - arpStartMs));
        arpFields.insert(QStringLiteral("cardIndex"), i + 1);
        recordDiagnosticEvent(QStringLiteral("network.probe"),
                              QStringLiteral("arp_probe"),
                              reachable ? DiagnosticRecorder::Severity::Info
                                        : DiagnosticRecorder::Severity::Debug,
                              arpFields);

        if (reachable) {
            // ARP 可达 → 卡已在线（重启上位机时卡不会重发18字节包）
            // 直接标记为就绪
            emit statusMessage(QString("卡%1（%2）ARP 探测成功，标记为就绪 ✓")
                              .arg(i + 1).arg(ipStr));
            if (i < static_cast<int>(m_cardDiagnostics.size()))
                ++m_cardDiagnostics[i].arpReady;
            markCardReady(i, false);   // ARP 探测就绪（并未收到 18 字节包）
            anyProbed = true;
        } else {
            emit statusMessage(QString("  卡%1（%2）ARP 探测不可达，%3 秒后重试...")
                              .arg(i + 1).arg(ipStr)
                              .arg(m_probeRetryCount < maxRetries ? "5" : "停止"));
        }
    }

    // 还有卡未就绪 → 5 秒后再次探测
    if (!isAllCardsReady()) {
        m_probeTimer->setSingleShot(true);
        m_probeTimer->start(5000);
    }
}

// ══ 卡就绪状态查询 ══════════════════════════════════════════════════
int NetworkController::readyCardCount() const
{
    return static_cast<int>(std::count(m_cardsReady.begin(), m_cardsReady.end(), true));
}

bool NetworkController::isCardReady(int cardIdx) const
{
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_cardsReady.size()))
        return false;
    return m_cardsReady[cardIdx];
}

bool NetworkController::isAllCardsReady() const
{
    return !m_cardsReady.empty() &&
           std::all_of(m_cardsReady.begin(), m_cardsReady.end(), [](bool v) { return v; });
}

// ══ 底层发送（不做就绪检查，由重试机制调用）════════════════════════════
bool NetworkController::sendRawToAll(const QByteArray& cmd,
                                      const QString& reason,
                                      int* outSuccess,
                                      int* outFail)
{
    int ok = 0, fail = 0;
    for (const QString& ip : m_targetIPs) {
        if (sendRawCommand(cmd, ip, reason)) ++ok;
        else ++fail;
    }
    if (outSuccess) *outSuccess = ok;
    if (outFail)    *outFail    = fail;
    return ok > 0;
}

bool NetworkController::doSendConfigCommand(int dataTime, int aDelay, int bDelay)
{
    QByteArray cmd = buildConfigPacket(dataTime, aDelay, bDelay);
    int successCount = 0, failCount = 0;
    sendRawToAll(cmd, QStringLiteral("first"), &successCount, &failCount);
    if (successCount > 0) {
        emit statusMessage(QString("配置命令已发送（%1/%2 张卡成功）: 采集=%3ns, A延时=%4ns, B延时=%5ns")
                          .arg(successCount).arg(m_targetIPs.size())
                          .arg(dataTime).arg(aDelay).arg(bDelay));
        if (failCount > 0)
            emit statusMessage(QString("另 %1 张发送失败，可能为尚未连接的卡").arg(failCount));
        return true;
    }
    return false;
}

bool NetworkController::doSendStartMeasureCard(int cardIndex,
                                                int* outSuccess,
                                                int* outFail)
{
    const unsigned char startBytes[58] = {
        0xFA, 0xFA, 0xFA, 0xFA, 0x03, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01
    };
    QByteArray cmd(reinterpret_cast<const char*>(startBytes), sizeof(startBytes));
    int successCount = 0, failCount = 0;
    if (cardIndex >= 0 && cardIndex < m_targetIPs.size()) {
        if (sendRawCommand(cmd, m_targetIPs[cardIndex], QStringLiteral("measure_start")))
            successCount = 1;
        else
            failCount = 1;
    } else {
        failCount = 1;
    }
    if (outSuccess) *outSuccess = successCount;
    if (outFail) *outFail = failCount;
    if (successCount != 1) {
        QJsonObject fence;
        fence.insert(QStringLiteral("measurementSessionId"), m_measurementSessionId);
        fence.insert(QStringLiteral("configId"), m_currentConfigId);
        fence.insert(QStringLiteral("cardIndex"), cardIndex);
        fence.insert(QStringLiteral("targetIP"),
                     cardIndex >= 0 && cardIndex < m_targetIPs.size()
                         ? m_targetIPs[cardIndex] : QString());
        fence.insert(QStringLiteral("localStartSendSucceeded"), false);
        fence.insert(QStringLiteral("startFenceStage"), QStringLiteral("send"));
        fence.insert(QStringLiteral("localStartSendSucceeded"), false);
        fence.insert(QStringLiteral("startFenceCompleteSucceeded"), false);
        fence.insert(QStringLiteral("startFenceCommitted"), false);
        recordDiagnosticEvent(QStringLiteral("network.measure"),
                              QStringLiteral("measurement_start_fence"),
                              DiagnosticRecorder::Severity::Error, fence);
    }
    if (successCount == 1 && failCount == 0) {
        emit statusMessage(QString("卡%1 开始测量命令已发送")
                          .arg(cardIndex + 1));
        return true;
    }
    if (successCount > 0)
        emit statusMessage(QString("卡%1 开始测量命令发送异常，事务将回滚")
                          .arg(cardIndex + 1));
    return false;
}

bool NetworkController::doSendStopMeasure(int* outSuccess, int* outFail)
{
    // 停止测量 = 命令0x03 + 末尾0x00（与开始测量仅末尾字节不同）
    const unsigned char stopBytes[58] = {
        0xFA, 0xFA, 0xFA, 0xFA, 0x03, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    QByteArray cmd(reinterpret_cast<const char*>(stopBytes), sizeof(stopBytes));
    int successCount = 0, failCount = 0;
    sendRawToAll(cmd, QStringLiteral("measure_stop"), &successCount, &failCount);
    if (outSuccess) *outSuccess = successCount;
    if (outFail) *outFail = failCount;
    if (successCount == m_targetIPs.size() && failCount == 0) {
        emit statusMessage(QString("停止测量命令已发送（%1/%2 张卡成功）")
                          .arg(successCount).arg(m_targetIPs.size()));
        return true;
    }
    if (successCount > 0)
        emit statusMessage(QString("停止测量命令部分成功（%1/%2）").arg(successCount).arg(m_targetIPs.size()));
    return false;
}

QString NetworkController::newMeasurementSessionId()
{
    m_measurementSessionToken = m_nextMeasurementSessionId++;
    return QStringLiteral("measurement-%1").arg(m_measurementSessionToken);
}

MeasurementSessionTransaction::TeardownResult
NetworkController::resetProcessorsAfterSession(bool hardwareStopSucceeded)
{
    return MeasurementSessionTransaction::teardown(
        static_cast<int>(m_receivers.size()),
        static_cast<int>(m_processors.size()),
        hardwareStopSucceeded,
        [this](int index) {
            return index >= 0 && index < static_cast<int>(m_receivers.size())
                && m_receivers[index]
                && m_receivers[index]->disarmSession(1500);
        },
        [this](int index) {
            return index >= 0 && index < static_cast<int>(m_processors.size())
                && m_processors[index]
                && m_processors[index]->disarmSession(1500);
        });
}

void NetworkController::recordSessionSettingsSnapshot(const QString& phase,
                                                       const QJsonObject& fields) const
{
    DiagnosticRecorder *recorder = diagnosticRecorder();
    if (!recorder) return;
    QJsonObject settings = fields;
    settings.insert(QStringLiteral("phase"), phase);
    settings.insert(QStringLiteral("bitsPerChannel"), m_config.bitsPerChannel);
    settings.insert(QStringLiteral("sampleIntervalNs"), m_config.sampleIntervalNs);
    settings.insert(QStringLiteral("acqTimeNs"), m_config.acqTimeNs);
    settings.insert(QStringLiteral("targetIPs"), QJsonArray::fromStringList(m_targetIPs.toList()));
    recorder->recordSettingsSnapshot(settings);
}

void NetworkController::clearPendingMeasurementRequests(const QString& reason)
{
    bool removed = false;
    for (auto it = m_cmdQueue.begin(); it != m_cmdQueue.end();) {
        if (it->type == PendingCmdType::StartMeasure ||
            it->type == PendingCmdType::StopMeasure) {
            it = m_cmdQueue.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (!m_measurementRunning && m_measurementState != MeasurementState::Fault)
        m_pendingMeasurementSessionId.clear();
    if (removed) {
        QJsonObject fields;
        fields.insert(QStringLiteral("configId"), m_currentConfigId);
        fields.insert(QStringLiteral("measurementSessionId"), m_pendingMeasurementSessionId);
        fields.insert(QStringLiteral("outcome"), QStringLiteral("cleared"));
        fields.insert(QStringLiteral("reason"), reason);
        recordDiagnosticEvent(QStringLiteral("network.measure"),
                              QStringLiteral("pending_session_cleared"),
                              DiagnosticRecorder::Severity::Warning, fields);
    }
}

void NetworkController::setMeasurementBoundaryFault(const QString& reason)
{
    m_measurementState = MeasurementState::Fault;
    m_measurementFaultReason = reason;
}

bool NetworkController::executeStartTransaction(const QString& requestedSessionId)
{
    const QString sessionId = requestedSessionId.isEmpty()
        ? newMeasurementSessionId() : requestedSessionId;
    if (m_measurementSessionToken == 0)
        m_measurementSessionToken = m_nextMeasurementSessionId++;
    const quint64 sessionToken = m_measurementSessionToken;
    m_measurementSessionId = sessionId;
    m_pendingMeasurementSessionId.clear();
    m_measurementFaultReason.clear();
    m_measurementState = MeasurementState::Preparing;

    QJsonObject common;
    common.insert(QStringLiteral("measurementSessionId"), sessionId);
    common.insert(QStringLiteral("configId"), m_currentConfigId);
    common.insert(QStringLiteral("cardCount"), m_targetIPs.size());
    common.insert(QStringLiteral("successCount"), 0);
    common.insert(QStringLiteral("failCount"), 0);
    common.insert(QStringLiteral("dataTimeNs"), m_config.acqTimeNs);
    common.insert(QStringLiteral("dataTime"), m_config.acqTimeNs);
    common.insert(QStringLiteral("processRunId"),
                 diagnosticRecorder() ? diagnosticRecorder()->runId() : QString());
    common.insert(QStringLiteral("runId"),
                 diagnosticRecorder() ? diagnosticRecorder()->runId() : QString());
    common.insert(QStringLiteral("trigger"), m_currentConfigTrigger);

    recordSessionSettingsSnapshot(QStringLiteral("measurement_session_prepare"), common);
    recordDiagnosticEvent(QStringLiteral("measurement.session"),
                          QStringLiteral("measurement_session_prepare"),
                          DiagnosticRecorder::Severity::Info, common);
    recordIngressSnapshot(QStringLiteral("measurement_session_prepare"));
    recordCardSnapshots(QStringLiteral("preparing"));

    bool hardwareAttempted = false;
    int startSuccessCount = 0;
    int startFailCount = 0;
    int receiverPrepareSuccessCount = 0;
    int receiverPrepareFailCount = 0;
    int receiverArmSuccessCount = 0;
    int receiverArmFailCount = 0;
    bool receiverStartFenceFailed = false;
    int startFenceBeginSuccessCount = 0;
    int startFenceBeginFailCount = 0;
    int startFenceCompleteSuccessCount = 0;
    int startFenceCompleteFailCount = 0;
    int startFenceCleanupSuccessCount = 0;
    int startFenceCommittedCount = 0;
    int startFenceFailedCount = 0;
    std::vector<bool> startFenceBeginResults(m_targetIPs.size(), false);
    std::vector<bool> startSendResults(m_targetIPs.size(), false);
    const auto admissionStateName = [](MultiPortReceiver::AdmissionState state) {
        switch (state) {
        case MultiPortReceiver::AdmissionState::Running:
            return QStringLiteral("running");
        case MultiPortReceiver::AdmissionState::StartFenceHold:
            return QStringLiteral("start_fence_hold");
        case MultiPortReceiver::AdmissionState::Armed:
            return QStringLiteral("armed");
        case MultiPortReceiver::AdmissionState::Preparing:
            return QStringLiteral("preparing");
        case MultiPortReceiver::AdmissionState::Disarmed:
            return QStringLiteral("disarmed");
        }
        return QStringLiteral("unknown");
    };
    const auto receiverAdmissionStateForCard = [this, &admissionStateName](int cardIndex) {
        for (const auto& receiver : m_receivers) {
            if (!receiver) continue;
            const auto snapshot = receiver->observabilitySnapshot();
            if (std::find(snapshot.cardIndices.begin(), snapshot.cardIndices.end(), cardIndex)
                != snapshot.cardIndices.end())
                return admissionStateName(receiver->cardAdmissionState(cardIndex));
        }
        return QStringLiteral("disarmed");
    };
    const auto rollback = [this, &hardwareAttempted, &startSuccessCount,
                           &startFailCount, &receiverPrepareSuccessCount,
                           &receiverPrepareFailCount, &receiverArmSuccessCount,
                           &receiverArmFailCount, &receiverStartFenceFailed,
                           &startFenceBeginSuccessCount, &startFenceBeginFailCount,
                           &startFenceCompleteSuccessCount, &startFenceCompleteFailCount,
                           &startFenceCleanupSuccessCount,
                           &startFenceCommittedCount, &startFenceFailedCount,
                           sessionId, common]() {
        int stopSuccess = 0;
        int stopFail = 0;
        if (hardwareAttempted)
            doSendStopMeasure(&stopSuccess, &stopFail);
        const bool hardwareStopSucceeded = !hardwareAttempted ||
            (stopSuccess == m_targetIPs.size() && stopFail == 0);
        const auto teardown = resetProcessorsAfterSession(hardwareStopSucceeded);
        QJsonObject fields = common;
        fields.insert(QStringLiteral("successCount"), startSuccessCount);
        fields.insert(QStringLiteral("failCount"), startFailCount);
        fields.insert(QStringLiteral("receiverPrepareSuccessCount"), receiverPrepareSuccessCount);
        fields.insert(QStringLiteral("receiverPrepareFailCount"), receiverPrepareFailCount);
        fields.insert(QStringLiteral("receiverArmSuccessCount"), receiverArmSuccessCount);
        fields.insert(QStringLiteral("receiverArmFailCount"), receiverArmFailCount);
        fields.insert(QStringLiteral("receiverStartFenceFailed"), receiverStartFenceFailed);
        fields.insert(QStringLiteral("receiverCommitFailed"), receiverStartFenceFailed);
        fields.insert(QStringLiteral("startFenceBeginSuccessCount"), startFenceBeginSuccessCount);
        fields.insert(QStringLiteral("startFenceBeginFailCount"), startFenceBeginFailCount);
        fields.insert(QStringLiteral("startFenceCompleteSuccessCount"), startFenceCompleteSuccessCount);
        fields.insert(QStringLiteral("startFenceCompleteFailCount"), startFenceCompleteFailCount);
        fields.insert(QStringLiteral("startFenceCleanupSuccessCount"), startFenceCleanupSuccessCount);
        fields.insert(QStringLiteral("startFenceCommittedCount"), startFenceCommittedCount);
        fields.insert(QStringLiteral("startFenceFailedCount"), startFenceFailedCount);
        fields.insert(QStringLiteral("rollbackStopSuccessCount"), stopSuccess);
        fields.insert(QStringLiteral("rollbackStopFailCount"), stopFail);
        fields.insert(QStringLiteral("receiverDisarmSuccessCount"), teardown.receiverSuccessCount);
        fields.insert(QStringLiteral("receiverDisarmFailCount"), teardown.receiverFailCount);
        fields.insert(QStringLiteral("processorDisarmSuccessCount"), teardown.processorSuccessCount);
        fields.insert(QStringLiteral("processorDisarmFailCount"), teardown.processorFailCount);
        fields.insert(QStringLiteral("teardownSuccess"), teardown.success);
        fields.insert(QStringLiteral("teardownReason"), teardown.reason);
        fields.insert(QStringLiteral("hardwareRollbackAttempted"), hardwareAttempted);
        fields.insert(QStringLiteral("outcome"), QStringLiteral("rolled_back"));
        fields.insert(QStringLiteral("reason"), teardown.success
                          ? QStringLiteral("start_transaction_failed")
                          : QStringLiteral("start_rollback_teardown_failed"));
        recordSessionSettingsSnapshot(QStringLiteral("measurement_start_rollback"), fields);
        recordDiagnosticEvent(QStringLiteral("measurement.session"),
                              QStringLiteral("measurement_start_rollback"),
                              DiagnosticRecorder::Severity::Error, fields);
        recordIngressSnapshot(QStringLiteral("measurement_start_rollback"));
        recordCardSnapshots(QStringLiteral("rollback"));
        if (DiagnosticRecorder *recorder = diagnosticRecorder())
            recorder->requestFlush();
        if (teardown.success) {
            m_measurementState = MeasurementState::Disarmed;
            m_measurementRunning = false;
            m_measurementSessionId.clear();
            m_measurementSessionToken = 0;
        } else {
            m_measurementRunning = true;
            setMeasurementBoundaryFault(teardown.reason);
        }
        emit measurementStartFailed(sessionId, QStringLiteral("start_transaction_failed"));
    };

    const MeasurementSessionTransaction::Result result =
        MeasurementSessionTransaction::start(
            static_cast<int>(m_processors.size()),
            static_cast<int>(m_targetIPs.size()),
            [this, sessionToken, &receiverPrepareSuccessCount,
             &receiverPrepareFailCount](int index) {
                if (index == 0) {
                    for (auto& receiver : m_receivers) {
                        if (receiver && receiver->prepareSession(sessionToken, 1500))
                            ++receiverPrepareSuccessCount;
                        else
                            ++receiverPrepareFailCount;
                    }
                    if (receiverPrepareFailCount != 0) return false;
                }
                return index >= 0 && index < static_cast<int>(m_processors.size())
                    && m_processors[index]
                    && m_processors[index]->prepareSession(sessionToken, 1500);
            },
            [this, sessionToken, &receiverArmSuccessCount,
             &receiverArmFailCount](int index) {
                if (index == 0) {
                    for (auto& receiver : m_receivers) {
                        if (receiver && receiver->armSession(sessionToken, 1500))
                            ++receiverArmSuccessCount;
                        else
                            ++receiverArmFailCount;
                    }
                    if (receiverArmFailCount != 0) return false;
                }
                return index >= 0 && index < static_cast<int>(m_processors.size())
                    && m_processors[index]
                    && m_processors[index]->armSession(sessionToken, 1500);
            },
            [this, &hardwareAttempted, &startSuccessCount, &startFailCount,
             &startSendResults](int cardIndex) {
                hardwareAttempted = true;
                MeasurementSessionTransaction::SendResult sent;
                doSendStartMeasureCard(cardIndex, &sent.successCount, &sent.failCount);
                startSuccessCount += sent.successCount;
                startFailCount += sent.failCount;
                startSendResults[static_cast<size_t>(cardIndex)] =
                    sent.successCount == 1 && sent.failCount == 0;
                return sent;
            },
            rollback,
            [this, common, &startSuccessCount, &startFailCount,
             &receiverPrepareSuccessCount, &receiverPrepareFailCount,
             &receiverArmSuccessCount, &receiverArmFailCount,
             &startFenceBeginSuccessCount, &startFenceBeginFailCount,
             &startFenceCompleteSuccessCount, &startFenceCompleteFailCount](const QString &step) mutable {
                if (step == QStringLiteral("measurement_session_prepare") ||
                    step == QStringLiteral("measurement_start_rollback") ||
                    step == QStringLiteral("measurement_started"))
                    return;
                QJsonObject fields = common;
                fields.insert(QStringLiteral("step"), step);
                fields.insert(QStringLiteral("successCount"), startSuccessCount);
                fields.insert(QStringLiteral("failCount"), startFailCount);
                fields.insert(QStringLiteral("receiverPrepareSuccessCount"), receiverPrepareSuccessCount);
                fields.insert(QStringLiteral("receiverPrepareFailCount"), receiverPrepareFailCount);
                fields.insert(QStringLiteral("receiverArmSuccessCount"), receiverArmSuccessCount);
                fields.insert(QStringLiteral("receiverArmFailCount"), receiverArmFailCount);
                fields.insert(QStringLiteral("startFenceBeginSuccessCount"), startFenceBeginSuccessCount);
                fields.insert(QStringLiteral("startFenceBeginFailCount"), startFenceBeginFailCount);
                fields.insert(QStringLiteral("startFenceCompleteSuccessCount"), startFenceCompleteSuccessCount);
                fields.insert(QStringLiteral("startFenceCompleteFailCount"), startFenceCompleteFailCount);
                if (step == QStringLiteral("measurement_session_armed")) {
                    m_measurementState = MeasurementState::Armed;
                    recordSessionSettingsSnapshot(step, fields);
                }
                recordDiagnosticEvent(QStringLiteral("measurement.session"), step,
                                      step == QStringLiteral("measurement_start_failed")
                                          ? DiagnosticRecorder::Severity::Error
                                          : DiagnosticRecorder::Severity::Info,
                                      fields);
            },
            [this, sessionId, sessionToken, &receiverStartFenceFailed,
             &startFenceBeginSuccessCount, &startFenceBeginFailCount,
             &startFenceCompleteSuccessCount, &startFenceCompleteFailCount,
             &startFenceCleanupSuccessCount, &startFenceCommittedCount,
             &startFenceFailedCount, &startFenceBeginResults,
             &receiverAdmissionStateForCard](int cardIndex) {
                const bool begun = [&] {
                    for (auto& receiver : m_receivers) {
                        if (receiver && receiver->beginCardStartFence(
                                sessionToken, cardIndex, 1500))
                            return true;
                    }
                    return false;
                }();
                startFenceBeginResults[static_cast<size_t>(cardIndex)] = begun;
                if (begun) {
                    ++startFenceBeginSuccessCount;
                    return true;
                }

                ++startFenceBeginFailCount;
                ++startFenceFailedCount;
                receiverStartFenceFailed = true;
                QJsonObject begin;
                begin.insert(QStringLiteral("measurementSessionId"), sessionId);
                begin.insert(QStringLiteral("configId"), m_currentConfigId);
                begin.insert(QStringLiteral("cardIndex"), cardIndex);
                begin.insert(QStringLiteral("targetIP"),
                             cardIndex >= 0 && cardIndex < m_targetIPs.size()
                                 ? m_targetIPs[cardIndex] : QString());
                begin.insert(QStringLiteral("startFenceStage"), QStringLiteral("begin"));
                begin.insert(QStringLiteral("startFenceBeginSucceeded"), false);
                begin.insert(QStringLiteral("localStartSendSucceeded"), false);
                begin.insert(QStringLiteral("startFenceCompleteSucceeded"), false);
                begin.insert(QStringLiteral("startFenceCommitted"), false);
                begin.insert(QStringLiteral("receiverAdmissionState"),
                             receiverAdmissionStateForCard(cardIndex));
                recordDiagnosticEvent(QStringLiteral("network.measure"),
                                      QStringLiteral("measurement_start_fence_begin"),
                                      DiagnosticRecorder::Severity::Error, begin);
                return false;
            },
            [this, sessionId, sessionToken, &receiverStartFenceFailed,
             &startFenceBeginResults, &startSendResults,
             &startFenceCompleteSuccessCount, &startFenceCompleteFailCount,
             &startFenceCleanupSuccessCount, &startFenceCommittedCount,
             &startFenceFailedCount, &receiverAdmissionStateForCard]
            (int cardIndex, bool startSucceeded) {
                bool completed = false;
                for (auto& receiver : m_receivers) {
                    if (receiver && receiver->completeCardStartFence(
                            sessionToken, cardIndex, startSucceeded, 1500)) {
                        completed = true;
                        break;
                    }
                }
                if (completed) {
                    ++startFenceCompleteSuccessCount;
                    if (startSucceeded)
                        ++startFenceCommittedCount;
                    else
                        ++startFenceCleanupSuccessCount;
                } else {
                    ++startFenceCompleteFailCount;
                    ++startFenceFailedCount;
                    receiverStartFenceFailed = true;
                }
                QJsonObject fence;
                fence.insert(QStringLiteral("measurementSessionId"), sessionId);
                fence.insert(QStringLiteral("configId"), m_currentConfigId);
                fence.insert(QStringLiteral("cardIndex"), cardIndex);
                fence.insert(QStringLiteral("targetIP"),
                             cardIndex >= 0 && cardIndex < m_targetIPs.size()
                                 ? m_targetIPs[cardIndex] : QString());
                fence.insert(QStringLiteral("startFenceStage"),
                             startSucceeded ? QStringLiteral("complete_success")
                                            : QStringLiteral("complete_failure_cleanup"));
                fence.insert(QStringLiteral("startFenceBeginSucceeded"),
                             static_cast<bool>(startFenceBeginResults[static_cast<size_t>(cardIndex)]));
                fence.insert(QStringLiteral("localStartSendSucceeded"),
                             static_cast<bool>(startSendResults[static_cast<size_t>(cardIndex)]));
                fence.insert(QStringLiteral("startFenceCompleteSucceeded"), completed);
                fence.insert(QStringLiteral("startFenceCommitted"),
                             startSucceeded && completed);
                fence.insert(QStringLiteral("receiverAdmissionState"),
                             receiverAdmissionStateForCard(cardIndex));
                recordDiagnosticEvent(QStringLiteral("network.measure"),
                                      QStringLiteral("measurement_start_fence"),
                                      startSucceeded && completed
                                          ? DiagnosticRecorder::Severity::Info
                                          : DiagnosticRecorder::Severity::Error,
                                      fence);
                return completed;
            });

    if (!result.success) {
        return false;
    }

    m_measurementRunning = true;
    m_measurementState = MeasurementState::Running;
    QJsonObject started = common;
    started.insert(QStringLiteral("successCount"), result.successCount);
    started.insert(QStringLiteral("failCount"), result.failCount);
    started.insert(QStringLiteral("outcome"), QStringLiteral("running"));
    started.insert(QStringLiteral("receiverPrepareSuccessCount"), receiverPrepareSuccessCount);
    started.insert(QStringLiteral("receiverPrepareFailCount"), receiverPrepareFailCount);
    started.insert(QStringLiteral("receiverArmSuccessCount"), receiverArmSuccessCount);
    started.insert(QStringLiteral("receiverArmFailCount"), receiverArmFailCount);
    started.insert(QStringLiteral("receiverStartFenceFailed"), receiverStartFenceFailed);
    started.insert(QStringLiteral("receiverCommitFailed"), receiverStartFenceFailed);
    started.insert(QStringLiteral("startFenceBeginSuccessCount"), result.startFenceBeginSuccessCount);
    started.insert(QStringLiteral("startFenceBeginFailCount"), result.startFenceBeginFailCount);
    started.insert(QStringLiteral("startFenceCompleteSuccessCount"), result.startFenceCompleteSuccessCount);
    started.insert(QStringLiteral("startFenceCompleteFailCount"), result.startFenceCompleteFailCount);
    started.insert(QStringLiteral("startFenceCleanupSuccessCount"), startFenceCleanupSuccessCount);
    started.insert(QStringLiteral("startFenceCommittedCount"), startFenceCommittedCount);
    started.insert(QStringLiteral("startFenceFailedCount"), startFenceFailedCount);
    recordSessionSettingsSnapshot(QStringLiteral("measurement_started"), started);
    recordDiagnosticEvent(QStringLiteral("measurement.session"),
                          QStringLiteral("measurement_started"),
                          DiagnosticRecorder::Severity::Info, started);
    recordIngressSnapshot(QStringLiteral("measurement_started"));
    recordCardSnapshots(QStringLiteral("running"));
    if (DiagnosticRecorder *recorder = diagnosticRecorder()) {
        recorder->requestFlush();
        recorder->flush(1500);
    }
    emit measurementStarted(sessionId);
    return true;
}

bool NetworkController::executeStopTransaction(const QString& requestedSessionId)
{
    const QString sessionId = requestedSessionId.isEmpty()
        ? m_measurementSessionId : requestedSessionId;
    m_measurementState = MeasurementState::Stopping;
    QJsonObject fields;
    fields.insert(QStringLiteral("measurementSessionId"), sessionId);
    fields.insert(QStringLiteral("configId"), m_currentConfigId);
    fields.insert(QStringLiteral("cardCount"), m_targetIPs.size());
    fields.insert(QStringLiteral("dataTimeNs"), m_config.acqTimeNs);
    fields.insert(QStringLiteral("dataTime"), m_config.acqTimeNs);
    fields.insert(QStringLiteral("processRunId"),
                  diagnosticRecorder() ? diagnosticRecorder()->runId() : QString());
    fields.insert(QStringLiteral("runId"),
                  diagnosticRecorder() ? diagnosticRecorder()->runId() : QString());
    fields.insert(QStringLiteral("successCount"), 0);
    fields.insert(QStringLiteral("failCount"), 0);
    fields.insert(QStringLiteral("outcome"), QStringLiteral("attempting"));
    fields.insert(QStringLiteral("reason"), QStringLiteral("stop_command"));
    recordSessionSettingsSnapshot(QStringLiteral("measurement_stop_command"), fields);
    recordDiagnosticEvent(QStringLiteral("measurement.session"),
                          QStringLiteral("measurement_stop_command"),
                          DiagnosticRecorder::Severity::Info, fields);

    int successCount = 0;
    int failCount = 0;
    const bool commandSucceeded = doSendStopMeasure(&successCount, &failCount);
    const auto teardown = resetProcessorsAfterSession(commandSucceeded);
    const bool clean = commandSucceeded && teardown.success;
    fields.insert(QStringLiteral("successCount"), successCount);
    fields.insert(QStringLiteral("failCount"), failCount);
    fields.insert(QStringLiteral("receiverDisarmSuccessCount"), teardown.receiverSuccessCount);
    fields.insert(QStringLiteral("receiverDisarmFailCount"), teardown.receiverFailCount);
    fields.insert(QStringLiteral("processorDisarmSuccessCount"), teardown.processorSuccessCount);
    fields.insert(QStringLiteral("processorDisarmFailCount"), teardown.processorFailCount);
    fields.insert(QStringLiteral("teardownSuccess"), teardown.success);
    fields.insert(QStringLiteral("teardownReason"), teardown.reason);
    fields.insert(QStringLiteral("outcome"), clean
                      ? QStringLiteral("stopped") : QStringLiteral("failed"));
    fields.insert(QStringLiteral("reason"), clean
                      ? QStringLiteral("teardown_complete") : teardown.reason);
    recordSessionSettingsSnapshot(clean ? QStringLiteral("measurement_stopped")
                                        : QStringLiteral("measurement_stop_failed"), fields);
    recordDiagnosticEvent(QStringLiteral("measurement.session"),
                          clean ? QStringLiteral("measurement_stopped")
                                : QStringLiteral("measurement_stop_failed"),
                          clean ? DiagnosticRecorder::Severity::Info
                                : DiagnosticRecorder::Severity::Error,
                          fields);
    // Hardware Stop is attempted before receiver/processor worker-owned
    // disarm/reset.  A failed command or barrier leaves an explicit fault;
    // the session id is retained so a later Stop can retry the same boundary.
    if (!clean) {
        m_measurementRunning = true;
        setMeasurementBoundaryFault(teardown.reason);
    } else {
        m_measurementRunning = false;
        m_measurementState = MeasurementState::Disarmed;
        m_measurementFaultReason.clear();
        m_measurementSessionId.clear();
        m_measurementSessionToken = 0;
        m_pendingMeasurementSessionId.clear();
    }
    recordIngressSnapshot(clean ? QStringLiteral("measurement_stopped")
                                : QStringLiteral("measurement_stop_failed"));
    recordCardSnapshots(clean ? QStringLiteral("stopped")
                              : QStringLiteral("stop_failed"));
    if (DiagnosticRecorder *recorder = diagnosticRecorder()) {
        recorder->requestFlush();
        recorder->flush(1500);
    }
    if (clean)
        emit measurementStopped(sessionId, true);
    else
        emit measurementStopFailed(sessionId, teardown.reason);
    return clean;
}

bool NetworkController::sendRawCommand(const QByteArray& cmd,
                                       const QString& targetIP,
                                       const QString& reason)
{
    QJsonObject fields;
    fields.insert(QStringLiteral("targetIP"), targetIP);
    fields.insert(QStringLiteral("targetPort"), CONTROL_PORT);
    fields.insert(QStringLiteral("payloadLength"), cmd.size());
    fields.insert(QStringLiteral("payloadHex"), NetworkDiagnostics::formatHex(cmd));
    fields.insert(QStringLiteral("reason"), reason);
    fields.insert(QStringLiteral("configId"), m_currentConfigId);
    fields.insert(QStringLiteral("trigger"), m_currentConfigTrigger);

    int cardIdx = m_targetIPs.indexOf(targetIP);
    if (cardIdx >= 0 && cardIdx < static_cast<int>(m_cardDiagnostics.size())) {
        ++m_cardDiagnostics[cardIdx].sendCount;
        fields.insert(QStringLiteral("sendCount"),
                      static_cast<double>(m_cardDiagnostics[cardIdx].sendCount));
    }

    if (m_controlSocket == INVALID_SOCKET) {
        fields.insert(QStringLiteral("sendtoReturn"), -1);
        fields.insert(QStringLiteral("errorCode"), QStringLiteral("socket_invalid"));
        recordDiagnosticEvent(QStringLiteral("network.control"),
                              QStringLiteral("send_skipped"),
                              DiagnosticRecorder::Severity::Error,
                              fields);
        return false;
    }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(CONTROL_PORT));
    const int addressResult = inet_pton(AF_INET, targetIP.toStdString().c_str(), &addr.sin_addr);
    int sent = sendto(m_controlSocket, cmd.constData(), cmd.size(), 0,
                      reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    int errorCode = 0;
    if (sent <= 0) {
#ifdef _WIN32
        errorCode = WSAGetLastError();
#else
        errorCode = errno;
#endif
    }
    fields.insert(QStringLiteral("addressParse"), addressResult == 1 ? QStringLiteral("ok")
                                                                       : QStringLiteral("failed"));
    fields.insert(QStringLiteral("sendtoReturn"), sent);
    fields.insert(QStringLiteral("errorCode"), errorCode);
    fields.insert(QStringLiteral("sendOutcome"), sent > 0 ? QStringLiteral("local_interface_ok")
                                                           : QStringLiteral("failed"));
    recordDiagnosticEvent(QStringLiteral("network.control"),
                          sent > 0 ? QStringLiteral("sendto_succeeded")
                                  : QStringLiteral("sendto_failed"),
                          sent > 0 ? DiagnosticRecorder::Severity::Info
                                  : DiagnosticRecorder::Severity::Warning,
                          fields);
    // 注意：不对每个 IP 单独 emit errorOccurred，避免 nCards 较大时日志刷屏。
    // 调用方（sendConfigCommand 等）统计成功/失败数后汇总上报。
    return (sent > 0);
}

// 构造配置包（58字节，与旧项目 buildConfigCommand 完全一致）
QByteArray NetworkController::buildConfigPacket(int dataTime, int aDelay, int bDelay)
{
    constexpr double FPGA_REG_NS = 4.0;
    int Trig        = static_cast<int>(bDelay   / FPGA_REG_NS);
    int ADC_capture = static_cast<int>(dataTime / FPGA_REG_NS);
    int adc_delay   = static_cast<int>(aDelay   / FPGA_REG_NS);

    QByteArray cmd;
    cmd.reserve(58);
    cmd.append('\xFA'); cmd.append('\xFA'); cmd.append('\xFA'); cmd.append('\xFA');
    cmd.append('\x02');
    cmd.append(static_cast<char>((Trig >> 24) & 0xFF));
    cmd.append(static_cast<char>((Trig >> 16) & 0xFF));
    cmd.append(static_cast<char>((Trig >>  8) & 0xFF));
    cmd.append(static_cast<char>( Trig        & 0xFF));
    cmd.append(static_cast<char>((ADC_capture >> 16) & 0xFF));
    cmd.append(static_cast<char>((ADC_capture >>  8) & 0xFF));
    cmd.append(static_cast<char>( ADC_capture        & 0xFF));
    cmd.append(static_cast<char>((adc_delay >> 16) & 0xFF));
    cmd.append(static_cast<char>((adc_delay >>  8) & 0xFF));
    cmd.append(static_cast<char>( adc_delay        & 0xFF));
    cmd.append(43, '\x00');
    return cmd;
}

bool NetworkController::sendConfigCommand(int dataTime,
                                          int aDelay,
                                          int bDelay,
                                          QString trigger)
{
    if(m_paimage)return configurePaimage(dataTime,aDelay,bDelay,trigger);
    const QString configId = QStringLiteral("config-%1").arg(m_nextConfigId++);
    if (trigger.isEmpty()) trigger = QStringLiteral("api");

    QJsonObject requestFields;
    requestFields.insert(QStringLiteral("configId"), configId);
    requestFields.insert(QStringLiteral("trigger"), trigger);
    requestFields.insert(QStringLiteral("dataTime"), dataTime);
    requestFields.insert(QStringLiteral("aDelay"), aDelay);
    requestFields.insert(QStringLiteral("bDelay"), bDelay);

    if (m_controlSocket == INVALID_SOCKET) {
        emit errorOccurred("控制 socket 未初始化，请先点击[开始监听]");
        requestFields.insert(QStringLiteral("reason"), QStringLiteral("socket_not_initialized"));
        recordDiagnosticEvent(QStringLiteral("network.config"),
                              QStringLiteral("config_rejected"),
                              DiagnosticRecorder::Severity::Error,
                              requestFields);
        return false;
    }

    if (isAllCardsReady()) {
        // ── 所有卡已就绪：逐卡下发配置并等待 60 字节反馈确认 ──
        requestFields.insert(QStringLiteral("outcome"), QStringLiteral("execute"));
        recordDiagnosticEvent(QStringLiteral("network.config"),
                              QStringLiteral("config_accepted"),
                              DiagnosticRecorder::Severity::Info,
                              requestFields);
        beginConfigWait({dataTime, aDelay, bDelay, configId, trigger});
        return true;
    }

    // ── 卡片未全部就绪：入队等待（队列保证配置不会被测量命令覆盖）──
    m_cmdQueue.push_back(PendingCmd{PendingCmdType::Config, dataTime, aDelay, bDelay,
                                    configId, trigger});

    int readyCnt = readyCardCount();
    emit statusMessage(QString("配置命令已加入等待队列（%1/%2 张卡就绪，等待全部就绪后自动发送...")
                      .arg(readyCnt).arg(m_targetIPs.size()));
    requestFields.insert(QStringLiteral("outcome"), QStringLiteral("queued"));
    requestFields.insert(QStringLiteral("readyCount"), readyCnt);
    recordDiagnosticEvent(QStringLiteral("network.config"),
                          QStringLiteral("config_queued"),
                          DiagnosticRecorder::Severity::Info,
                          requestFields);

    // 启动重试定时器（如果还没启动）
    if (m_retryTimer && !m_retryTimer->isActive())
        m_retryTimer->start();

    return true;  // 返回 true 表示命令已接受
}

bool NetworkController::sendStartMeasure()
{
    if(m_paimage)return startPaimageMeasurement();
    if (m_controlSocket == INVALID_SOCKET) {
        emit errorOccurred("控制 socket 未初始化，请先点击[开始监听]");
        QJsonObject fields;
        fields.insert(QStringLiteral("command"), QStringLiteral("start"));
        fields.insert(QStringLiteral("outcome"), QStringLiteral("rejected"));
        fields.insert(QStringLiteral("reason"), QStringLiteral("socket_not_initialized"));
        recordDiagnosticEvent(QStringLiteral("network.measure"),
                              QStringLiteral("command_rejected"),
                              DiagnosticRecorder::Severity::Error,
                              fields);
        return false;
    }

    if (m_measurementRunning || !m_pendingMeasurementSessionId.isEmpty()) {
        emit errorOccurred("已有测量会话正在运行或等待启动");
        return false;
    }
    if (m_measurementState == MeasurementState::Fault ||
        m_measurementState == MeasurementState::Stopping ||
        m_measurementState == MeasurementState::Preparing ||
        m_measurementState == MeasurementState::Armed) {
        emit errorOccurred(QString("测量边界处于故障/过渡状态：%1")
                           .arg(m_measurementFaultReason.isEmpty()
                                    ? QStringLiteral("session_boundary_not_clean")
                                    : m_measurementFaultReason));
        return false;
    }
    const QString measurementSessionId = newMeasurementSessionId();
    m_pendingMeasurementSessionId = measurementSessionId;

    if (isAllCardsReady()) {
        if (isConfigConfirmed()) {
            // ── 配置已确认：直接发送（测量命令无反馈，不等待）──
            const bool result = executeStartTransaction(measurementSessionId);
            QJsonObject fields;
            fields.insert(QStringLiteral("command"), QStringLiteral("start"));
            fields.insert(QStringLiteral("configId"), m_currentConfigId);
            fields.insert(QStringLiteral("measurementSessionId"), measurementSessionId);
            fields.insert(QStringLiteral("outcome"),
                          result ? QStringLiteral("executed") : QStringLiteral("rejected"));
            fields.insert(QStringLiteral("reason"),
                          result ? QStringLiteral("config_confirmed")
                                : QStringLiteral("send_failed"));
            recordDiagnosticEvent(QStringLiteral("network.measure"),
                                  result ? QStringLiteral("command_executed")
                                         : QStringLiteral("command_rejected"),
                                  result ? DiagnosticRecorder::Severity::Info
                                         : DiagnosticRecorder::Severity::Error,
                                  fields);
            return result;
        }
        if (m_configPhase == ConfigPhase::Failed) {
            emit errorOccurred("配置未确认（存在失败卡），无法开始测量，请重新下发配置");
            QJsonObject fields;
            fields.insert(QStringLiteral("command"), QStringLiteral("start"));
            fields.insert(QStringLiteral("configId"), m_currentConfigId);
            fields.insert(QStringLiteral("measurementSessionId"), measurementSessionId);
            fields.insert(QStringLiteral("outcome"), QStringLiteral("rejected"));
            fields.insert(QStringLiteral("reason"), QStringLiteral("config_failed"));
            recordDiagnosticEvent(QStringLiteral("network.measure"),
                                  QStringLiteral("command_rejected"),
                                  DiagnosticRecorder::Severity::Error,
                                  fields);
            m_pendingMeasurementSessionId.clear();
            return false;
        }
        // 等待配置确认完成后自动发送（configConfirmed 会触发 retryPendingCommand）
        m_cmdQueue.push_back(PendingCmd{PendingCmdType::StartMeasure, 0, 0, 0,
                                        QString(), QStringLiteral("api"), measurementSessionId});
        emit statusMessage("等待配置参数确认完成后自动开始测量...");
        QJsonObject fields;
        fields.insert(QStringLiteral("command"), QStringLiteral("start"));
        fields.insert(QStringLiteral("configId"), m_currentConfigId);
        fields.insert(QStringLiteral("measurementSessionId"), measurementSessionId);
        fields.insert(QStringLiteral("outcome"), QStringLiteral("queued"));
        fields.insert(QStringLiteral("reason"), QStringLiteral("waiting_config_ack"));
        recordDiagnosticEvent(QStringLiteral("network.measure"),
                              QStringLiteral("command_queued"),
                              DiagnosticRecorder::Severity::Info,
                              fields);
        if (m_retryTimer && !m_retryTimer->isActive())
            m_retryTimer->start();
        return true;
    }

    // ── 卡片未全部就绪：入队等待（队列保证顺序）──
    m_cmdQueue.push_back(PendingCmd{PendingCmdType::StartMeasure, 0, 0, 0,
                                    QString(), QStringLiteral("api"), measurementSessionId});

    int readyCnt = readyCardCount();
    emit statusMessage(QString("开始测量命令已加入等待队列（%1/%2 张卡就绪，等待全部就绪后自动发送...")
                      .arg(readyCnt).arg(m_targetIPs.size()));
    QJsonObject fields;
    fields.insert(QStringLiteral("command"), QStringLiteral("start"));
    fields.insert(QStringLiteral("configId"), m_currentConfigId);
    fields.insert(QStringLiteral("measurementSessionId"), measurementSessionId);
    fields.insert(QStringLiteral("outcome"), QStringLiteral("queued"));
    fields.insert(QStringLiteral("reason"), QStringLiteral("waiting_ready"));
    fields.insert(QStringLiteral("readyCount"), readyCnt);
    recordDiagnosticEvent(QStringLiteral("network.measure"),
                          QStringLiteral("command_queued"),
                          DiagnosticRecorder::Severity::Info,
                          fields);

    if (m_retryTimer && !m_retryTimer->isActive())
        m_retryTimer->start();

    return true;
}

bool NetworkController::sendStopMeasure()
{
    if(m_paimage)return stopPaimageMeasurement();
    if (m_controlSocket == INVALID_SOCKET) {
        emit errorOccurred("控制 socket 未初始化，请先点击[开始监听]");
        QJsonObject fields;
        fields.insert(QStringLiteral("command"), QStringLiteral("stop"));
        fields.insert(QStringLiteral("outcome"), QStringLiteral("rejected"));
        fields.insert(QStringLiteral("reason"), QStringLiteral("socket_not_initialized"));
        recordDiagnosticEvent(QStringLiteral("network.measure"),
                              QStringLiteral("command_rejected"),
                              DiagnosticRecorder::Severity::Error,
                              fields);
        return false;
    }

    const QString measurementSessionId = m_measurementSessionId.isEmpty()
        ? m_pendingMeasurementSessionId : m_measurementSessionId;

    if (isAllCardsReady()) {
        if (isConfigConfirmed()) {
            // ── 配置已确认：直接发送（测量命令无反馈，不等待）──
            const bool result = executeStopTransaction(measurementSessionId);
            QJsonObject fields;
            fields.insert(QStringLiteral("command"), QStringLiteral("stop"));
            fields.insert(QStringLiteral("configId"), m_currentConfigId);
            fields.insert(QStringLiteral("measurementSessionId"), measurementSessionId);
            fields.insert(QStringLiteral("outcome"),
                          result ? QStringLiteral("executed") : QStringLiteral("rejected"));
            fields.insert(QStringLiteral("reason"),
                          result ? QStringLiteral("config_confirmed")
                                : QStringLiteral("send_failed"));
            recordDiagnosticEvent(QStringLiteral("network.measure"),
                                  result ? QStringLiteral("command_executed")
                                         : QStringLiteral("command_rejected"),
                                  result ? DiagnosticRecorder::Severity::Info
                                         : DiagnosticRecorder::Severity::Error,
                                  fields);
            return result;
        }
        if (m_configPhase == ConfigPhase::Failed) {
            emit errorOccurred("配置未确认（存在失败卡），无法停止测量");
            QJsonObject fields;
            fields.insert(QStringLiteral("command"), QStringLiteral("stop"));
            fields.insert(QStringLiteral("configId"), m_currentConfigId);
            fields.insert(QStringLiteral("outcome"), QStringLiteral("rejected"));
            fields.insert(QStringLiteral("reason"), QStringLiteral("config_failed"));
            recordDiagnosticEvent(QStringLiteral("network.measure"),
                                  QStringLiteral("command_rejected"),
                                  DiagnosticRecorder::Severity::Error,
                                  fields);
            return false;
        }
        // 等待配置确认完成后自动发送（configConfirmed 会触发 retryPendingCommand）
        m_cmdQueue.push_back(PendingCmd{PendingCmdType::StopMeasure, 0, 0, 0,
                                        QString(), QStringLiteral("api"), measurementSessionId});
        emit statusMessage("等待配置参数确认完成后自动停止测量...");
        QJsonObject fields;
        fields.insert(QStringLiteral("command"), QStringLiteral("stop"));
        fields.insert(QStringLiteral("configId"), m_currentConfigId);
        fields.insert(QStringLiteral("measurementSessionId"), measurementSessionId);
        fields.insert(QStringLiteral("outcome"), QStringLiteral("queued"));
        fields.insert(QStringLiteral("reason"), QStringLiteral("waiting_config_ack"));
        recordDiagnosticEvent(QStringLiteral("network.measure"),
                              QStringLiteral("command_queued"),
                              DiagnosticRecorder::Severity::Info,
                              fields);
        if (m_retryTimer && !m_retryTimer->isActive())
            m_retryTimer->start();
        return true;
    }

    m_cmdQueue.push_back(PendingCmd{PendingCmdType::StopMeasure, 0, 0, 0,
                                    QString(), QStringLiteral("api"), measurementSessionId});
    int readyCnt = readyCardCount();
    emit statusMessage(QString("停止测量命令已加入等待队列（%1/%2 张卡就绪，等待全部就绪后自动发送...")
                      .arg(readyCnt).arg(m_targetIPs.size()));
    QJsonObject fields;
    fields.insert(QStringLiteral("command"), QStringLiteral("stop"));
    fields.insert(QStringLiteral("configId"), m_currentConfigId);
    fields.insert(QStringLiteral("measurementSessionId"), measurementSessionId);
    fields.insert(QStringLiteral("outcome"), QStringLiteral("queued"));
    fields.insert(QStringLiteral("reason"), QStringLiteral("waiting_ready"));
    fields.insert(QStringLiteral("readyCount"), readyCnt);
    recordDiagnosticEvent(QStringLiteral("network.measure"),
                          QStringLiteral("command_queued"),
                          DiagnosticRecorder::Severity::Info,
                          fields);
    if (m_retryTimer && !m_retryTimer->isActive())
        m_retryTimer->start();
    return true;
}

void NetworkController::setMeasureEnabled(bool enable)
{
    if(m_paimage){if(enable)startPaimageMeasurement();else stopPaimageMeasurement();return;}
    for (auto& receiver : m_receivers) {
        if (receiver) receiver->setCompatibilityAdmission(enable, 1);
    }
    for (auto &p : m_processors) {
        if (p) p->setMeasureEnabled(enable);
    }
}

// ═════════════════════════════════════════════════════════════════════
// 网段扫描自动识别
//
// 专用网段假设：192.168.0.x 范围内可达的 IP 即采集卡（上位机与交换机
// 的 IP 会避开扫描范围）。扫描用于自动确定采集卡数量与目标 IP，
// 减少用户手动配置。扫描范围由注册表
//   NetworkParams/ScanBaseIP（默认 192.168.0.2）
//   NetworkParams/ScanIPCount（默认 32）
// 控制。
// ═════════════════════════════════════════════════════════════════════
#ifdef _WIN32
// ICMP 快扫（采集卡响应 ping）：短超时，比 SendARP 快一个数量级
static bool icmpProbeOnce(HANDLE hIcmp,
                          const QString& ip,
                          DWORD timeoutMs,
                          bool* timedOut,
                          DWORD* replyCount,
                          DWORD* errorCode,
                          DWORD* replyStatus)
{
    IPAddr dest = inet_addr(ip.toStdString().c_str());
    char sendData[32] = {0};
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + 64;
    std::vector<char> replyBuf(replySize, 0);
    DWORD n = IcmpSendEcho(hIcmp, dest, sendData, sizeof(sendData), nullptr,
                           replyBuf.data(), replySize, timeoutMs);
    if (replyCount) *replyCount = n;
    if (n > 0) {
        if (replyStatus) *replyStatus = reinterpret_cast<const ICMP_ECHO_REPLY*>(replyBuf.data())->Status;
        *timedOut = false;
        return true; // Preserve the existing scan criterion; log the status separately.
    }
    DWORD err = GetLastError();
    if (errorCode) *errorCode = err;
    *timedOut = (err == IP_REQ_TIMED_OUT);   // 超时=离线，不视为探测错误
    return false;
}
// ARP 兜底（ICMP 未命中时，兼容不响应 ping 或 ICMP 被过滤的情况）
static bool arpProbeOnce(const QString& ip, QString* macText, DWORD* errorCode)
{
    ULONG macBuf[2] = {};
    ULONG macLen = sizeof(macBuf);
    IPAddr dest = inet_addr(ip.toStdString().c_str());
    DWORD arpRet = SendARP(dest, 0, macBuf, &macLen);
    if (errorCode) *errorCode = arpRet;
    if (macText && arpRet == NO_ERROR && macLen >= 6) {
        const auto *mac = static_cast<const uint8_t*>(static_cast<const void*>(macBuf));
        *macText = QStringLiteral("%1:%2:%3:%4:%5:%6")
            .arg(mac[0], 2, 16, QLatin1Char('0'))
            .arg(mac[1], 2, 16, QLatin1Char('0'))
            .arg(mac[2], 2, 16, QLatin1Char('0'))
            .arg(mac[3], 2, 16, QLatin1Char('0'))
            .arg(mac[4], 2, 16, QLatin1Char('0'))
            .arg(mac[5], 2, 16, QLatin1Char('0')).toUpper();
    }
    return arpRet == NO_ERROR && macLen >= 6;
}
#endif

QVector<QString> NetworkController::scanReachableIPs(const QString& baseIP,
                                                     int count,
                                                     const QString& listenId)
{
    QVector<QString> result;
    const QStringList parts = baseIP.split('.');
    if (parts.size() != 4) return result;

    bool ok = false;
    const int baseOctet = parts[3].toInt(&ok);
    if (!ok) return result;
    const QString prefix = parts[0] + "." + parts[1] + "." + parts[2] + ".";

    // 生成候选 IP 列表
    std::vector<QString> ips;
    for (int i = 0; i < count; ++i) {
        const int octet = baseOctet + i;
        if (octet < 1 || octet > 254) break;
        ips.push_back(prefix + QString::number(octet));
    }
    const int total = static_cast<int>(ips.size());
    if (total <= 0) return result;

    QStringList candidates;
    for (const auto &ip : ips) candidates.append(ip);
    if (auto *r = diagnosticRecorder()) {
        r->recordEvent("scan.begin", "开始扫描候选地址", DiagnosticRecorder::Severity::Info,
            {{"listenId",listenId},{"baseIP",baseIP},{"requestedCount",count},
             {"candidates",QJsonArray::fromStringList(candidates)}});
    }
    (void)QtConcurrent::run([candidates,listenId] {
        auto snapshot = NetworkDiagnostics::collectSnapshot(candidates);
        snapshot.insert("listenId",listenId);
        snapshot.insert("reason","scan_start");
        if (auto *r = diagnosticRecorder()) r->recordNetworkSnapshot(snapshot);
    });

    // 并行探测：最多 16 线程分片，每线程只写自己的槽位（无竞争）。
    // ICMP 快扫（150ms 超时）为主判据：在线毫秒级响应，超时=离线直接判离线，
    // 只有 ICMP 调用出错（非超时）才用 SendARP 兜底，避免离线 IP 叠加 ARP 超时。
    std::vector<char> online(total, 0);
    const int nThreads = std::min<int>(16, total);
    std::vector<std::thread> workers;
    workers.reserve(nThreads);
    for (int t = 0; t < nThreads; ++t) {
        workers.emplace_back([&ips, &online, t, nThreads, listenId]() {
#ifdef _WIN32
            HANDLE hIcmp = IcmpCreateFile();
            const DWORD createError = hIcmp == INVALID_HANDLE_VALUE ? GetLastError() : 0;
            if (hIcmp == INVALID_HANDLE_VALUE) hIcmp = nullptr;
            for (int i = t; i < static_cast<int>(ips.size()); i += nThreads) {
                const auto started = std::chrono::steady_clock::now();
                bool timedOut = false;
                DWORD replies=0, icmpError=createError, replyStatus=0, arpError=0;
                QString mac;
                bool reachable = (hIcmp && icmpProbeOnce(hIcmp, ips[i], SCAN_ICMP_TIMEOUT_MS,
                    &timedOut, &replies, &icmpError, &replyStatus));
                const bool arpAttempted = !reachable && !timedOut;
                if (arpAttempted) reachable = arpProbeOnce(ips[i], &mac, &arpError);
                online[i] = reachable ? 1 : 0;
                const double elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
                if (auto *r=diagnosticRecorder()) r->recordEvent("scan.result", "候选地址探测结果",
                    DiagnosticRecorder::Severity::Info,
                    {{"listenId",listenId},{"ip",ips[i]},{"candidateIndex",i+1},
                     {"reachable",reachable},{"elapsedMs",elapsed},{"icmpAttempted",hIcmp!=nullptr},
                     {"icmpReplyCount",double(replies)},{"icmpReplyStatus",replies ? QJsonValue(double(replyStatus)):QJsonValue()},
                     {"icmpError",double(icmpError)},{"icmpTimedOut",timedOut},
                     {"arpAttempted",arpAttempted},{"arpError",arpAttempted?QJsonValue(double(arpError)):QJsonValue()},
                     {"mac",mac},{"selectionBasis",reachable?(arpAttempted?"arp_success":"icmp_reply_count"):"not_selected"}});
            }
            if (hIcmp) IcmpCloseHandle(hIcmp);
#else
            (void)ips;
            for (int i = t; i < static_cast<int>(ips.size()); i += nThreads)
                online[i] = 0;
#endif
        });
    }
    for (auto& w : workers) w.join();

    // 按原顺序收集在线 IP
    for (int i = 0; i < total; ++i)
        if (online[i]) result.append(ips[i]);
    if (auto *r=diagnosticRecorder()) r->recordEvent("scan.complete", "扫描完成，有序目标列表",
        DiagnosticRecorder::Severity::Info, {{"listenId",listenId},{"targetIPs",QJsonArray::fromStringList(result)},
                                           {"targetCount",result.size()}});
    return result;
}
