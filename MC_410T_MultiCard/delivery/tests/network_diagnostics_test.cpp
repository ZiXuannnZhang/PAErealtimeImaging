#include "NetworkController.h"
#include "DiagnosticRecorder.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QTextStream>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <ws2tcpip.h>

namespace {
struct Cards {
    QVector<SOCKET> sockets;
    QVector<int> configs;
    ~Cards() { for (SOCKET s : sockets) closesocket(s); }
    bool open(int n) {
        configs.fill(0, n);
        for (int i=0;i<n;++i) {
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if(s==INVALID_SOCKET) return false;
            sockets.append(s);
            sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(8080);
            inet_pton(AF_INET, qPrintable(QString("127.0.0.%1").arg(i+1)), &addr.sin_addr);
            if(bind(s,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0) return false;
            u_long nonblocking=1; ioctlsocket(s,FIONBIO,&nonblocking);
        }
        return true;
    }
    static void feedback(SOCKET s, int length) {
        sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(8000);
        inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
        QByteArray bytes(length,'\0');
        sendto(s,bytes.constData(),bytes.size(),0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    }
    void pump(bool fifthFails) {
        for(int i=0;i<sockets.size();++i) {
            char bytes[128]; sockaddr_in source{}; int len=sizeof(source);
            int n=recvfrom(sockets[i],bytes,sizeof(bytes),0,reinterpret_cast<sockaddr*>(&source),&len);
            if(n==58 && static_cast<unsigned char>(bytes[4])==2) {
                ++configs[i];
                if(fifthFails && i==4) continue;
                if(i==0 && configs[i]==1) feedback(sockets[i],18);
                feedback(sockets[i],60);
                feedback(sockets[i],60); // duplicate evidence
            }
        }
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
};
bool require(bool yes, const QString &text) {
    if(!yes) QTextStream(stderr) << "FAIL " << text << Qt::endl;
    return yes;
}

bool runtimeStatsFieldMapping()
{
    CardStats::Snapshot stats;
    stats.packetsReceived = 900;
    stats.packetsDropped = 7;
    stats.triggersComplete = 12;
    stats.triggersPartial = 3;
    stats.missingTriggerCount = 11;
    stats.triggersDiscarded = 2;
    stats.saveQueueDiscards = 1;
    stats.inputQueueDepth = 100;
    stats.saveQueueDepth = 5;
    stats.recvMbps = 123.45;
    stats.triggerHz = 99.5;
    stats.packetLossRate = 0.007;
    stats.socketPacketsReceived = 1000;
    stats.processorPacketsDequeued = 900;
    stats.batchBoundaryDiscards = 0;
    stats.sameTriggerForwardGapEvents = 2;
    stats.sameTriggerForwardGapPackets = 5;
    stats.sameTriggerBackstepEvents = 1;
    stats.sameTriggerDuplicateSeqEvents = 3;
    stats.crossTriggerLateArrivalEvents = 4;
    stats.staleTriggerPacketsDiscarded = 6;
    stats.assemblyDuplicatePackets = 7;
    stats.assemblyOffsetOutOfRangePackets = 8;
    stats.sessionBoundaryPacketsDiscarded = 9;
    stats.lastTriggerSeq = 12;
    stats.lastPacketSeq = 34;
    stats.rawSequenceInitialized = true;
    const QJsonObject fields = NetworkController::runtimeStatsFields(stats);
    bool ok = require(fields.value("socketPacketsReceived").toDouble() == 1000,
                      "runtime socket counter mapping");
    ok = require(fields.value("processorPacketsDequeued").toDouble() == 900,
                 "runtime processor counter mapping") && ok;
    ok = require(fields.value("batchBoundaryDiscards").toDouble() == 0,
                 "runtime batch boundary mapping") && ok;
    ok = require(fields.value("sameTriggerForwardGapEvents").toDouble() == 2
                     && fields.value("sameTriggerForwardGapPackets").toDouble() == 5
                     && fields.value("staleTriggerPacketsDiscarded").toDouble() == 6
                     && fields.value("assemblyOffsetOutOfRangePackets").toDouble() == 8
                     && fields.value("sessionBoundaryPacketsDiscarded").toDouble() == 9,
                 "runtime ingress/rejection field mapping") && ok;
    ok = require(fields.value("packetsDropped").toDouble() == 7
                     && fields.value("triggersPartial").toDouble() == 3
                     && fields.value("inputQueueDepth").toInt() == 100
                     && fields.value("saveQueueDepth").toInt() == 5,
                 "legacy runtime field mapping") && ok;
    ok = require(fields.value("missingTriggerCount").toDouble() == 11,
                 "runtime missingTriggerCount mapping") && ok;
    return ok;
}

// Session B：物理轮次启动策略经 NetworkController production setter 传播并
// 可由 physicalRoundSnapshot() 反映（backend 未创建时经成员回退路径）。
// 与 RoundPolicySettings helper 组合覆盖“保存 default 后未打开对话框也能
// 在启动时获得已保存 policy”与“Apply/OK 后更新 controller policy”两条链路。
bool roundPolicyPropagation()
{
    NetworkController controller;
    // 出厂/无历史默认与 Session A 编译期默认一致：1 / false
    bool ok = require(controller.physicalRoundSnapshot().startupFilterTriggerCount == 1
                          && !controller.physicalRoundSnapshot().disableCountBoundary,
                      "controller default policy 1/false");
    // Apply/OK 转发路径调用的正是这两个 setter
    controller.setStartupFilterTriggerCount(7);
    controller.setDisableCountBoundary(true);
    const auto applied = controller.physicalRoundSnapshot();
    ok = require(applied.startupFilterTriggerCount == 7 && applied.disableCountBoundary,
                 "setter policy reflected in snapshot") && ok;
    // 参数独立性：X=0 不改变 disable；disable=false 不改变 X
    controller.setStartupFilterTriggerCount(0);
    const auto zeroFilter = controller.physicalRoundSnapshot();
    ok = require(zeroFilter.startupFilterTriggerCount == 0 && zeroFilter.disableCountBoundary,
                 "X=0 independent of disableCountBoundary") && ok;
    controller.setStartupFilterTriggerCount(7);
    controller.setDisableCountBoundary(false);
    const auto enabledBoundary = controller.physicalRoundSnapshot();
    ok = require(enabledBoundary.startupFilterTriggerCount == 7
                     && !enabledBoundary.disableCountBoundary,
                 "disable=false independent of X=7") && ok;
    return ok;
}

bool receptionDuringExport(bool withExport, const QString &output)
{
    NetworkController controller;
    AcqConfig config;
    config.nCards=4; config.acqTimeNs=16000; config.localBindIP="127.0.0.1";
    config.targetIPs={"127.0.0.1","127.0.0.2","127.0.0.3","127.0.0.4"};
    controller.setDiagnosticContext(withExport ? "replay-export" : "replay-baseline", "test_loopback");
    if(!require(controller.start(config),"reception start")) return false;
    controller.setMeasureEnabled(true);
    SOCKET sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    std::future<DiagnosticRecorder::ExportResult> job;
    QElapsedTimer timer; timer.start();
    constexpr int triggers=80;
    const int bytesPerTrigger=config.samplesPerTrig()*config.bytesPerSamplePair();
    for(int t=0;t<triggers;++t) {
        while(timer.elapsed()<t*25) { QCoreApplication::processEvents(); QThread::msleep(1); }
        if(withExport && t==40) {
            auto *r=DiagnosticRecorder::instance();
            for(int i=0;i<2000;++i) r->recordEvent("export-load",QString::number(i));
            job=r->exportRunAsync(output);
        }
        for(int card=0;card<4;++card) {
            sockaddr_in to{}; to.sin_family=AF_INET; to.sin_port=htons(8001+card);
            inet_pton(AF_INET,"127.0.0.1",&to.sin_addr);
            for(int p=0;p<config.packetsPerTrig();++p) {
                const int length=qMin(UDP_PAYLOAD_BYTES,bytesPerTrigger-p*UDP_PAYLOAD_BYTES);
                QByteArray packet(4+length,'\0');
                quint16 seq=static_cast<quint16>(t*config.packetsPerTrig()+p);
                packet[0]=seq&255; packet[1]=(seq>>8)&255; packet[2]=t&255; packet[3]=(t>>8)&255;
                if(sendto(sender,packet.constData(),packet.size(),0,reinterpret_cast<sockaddr*>(&to),sizeof(to))!=packet.size()) {
                    closesocket(sender); return require(false,"replay send failed");
                }
            }
        }
    }
    closesocket(sender);
    QElapsedTimer settle; settle.start();
    while(settle.elapsed()<400) { QCoreApplication::processEvents(); QThread::msleep(2); }
    const auto stats=controller.getAllCardStats();
    bool ok=stats.size()==4;
    quint64 totalSocket = 0;
    quint64 totalDequeued = 0;
    quint64 totalBoundaryDiscards = 0;
    int finalQueueDepth = 0;
    quint64 totalDropped = 0;
    quint64 totalPartial = 0;
    for(const auto &card:stats) {
        ok=require(card.triggersComplete==triggers && card.packetsDropped==0 && card.triggersPartial==0,
            QString("replay card=%1 triggers=%2 dropped=%3 partial=%4")
                .arg(card.cardId).arg(card.triggersComplete).arg(card.packetsDropped).arg(card.triggersPartial)) && ok;
        ok=require(card.socketPacketsReceived >= card.processorPacketsDequeued
                       && card.batchBoundaryDiscards == 0,
                   QString("replay card=%1 socket=%2 dequeued=%3 boundary=%4")
                       .arg(card.cardId)
                       .arg(card.socketPacketsReceived)
                       .arg(card.processorPacketsDequeued)
                       .arg(card.batchBoundaryDiscards)) && ok;
        totalSocket += card.socketPacketsReceived;
        totalDequeued += card.processorPacketsDequeued;
        totalBoundaryDiscards += card.batchBoundaryDiscards;
        finalQueueDepth += card.inputQueueDepth;
        totalDropped += card.packetsDropped;
        totalPartial += card.triggersPartial;
    }
    if(withExport) ok=require(job.get().success,"export during reception") && ok;
    if (ok) QTextStream(stdout) << "PASS replay " << (withExport?"with export":"baseline")
                               << ": 4 cards x 80 triggers, 40Hz"
                               << " socketPacketsReceived=" << totalSocket
                               << " processorPacketsDequeued=" << totalDequeued
                               << " batchBoundaryDiscards=" << totalBoundaryDiscards
                               << " inputQueueDepth=" << finalQueueDepth
                               << " packetsDropped=" << totalDropped
                               << " triggersPartial=" << totalPartial << Qt::endl;
    return ok;
}
}

class NetworkDiagnosticTestAccess {
public:
    static bool exercise(int n) {
        Cards cards;
        if(!require(cards.open(n),"loopback ports 8080 unavailable")) return false;
        NetworkController c;
        c.setDiagnosticContext(QString("test-%1-cards").arg(n),"test_explicit_loopback");
        c.m_config.nCards=n;
        c.m_config.localBindIP="127.0.0.1";
        for(int i=0;i<n;++i) c.m_targetIPs.append(QString("127.0.0.%1").arg(i+1));
        c.m_cardsReady.assign(n,false);
        c.m_cardDiagnostics.resize(n);
        if(!require(c.initControlSocket() && c.initFeedbackListener(),"control/feedback startup")) return false;
        for(int i=0;i<qMin(n,4);++i) Cards::feedback(cards.sockets[i],18);
        if(n==5) c.markCardReady(4,false); // exercise the existing ARP-ready branch without sending ARP
        QElapsedTimer timer; timer.start();
        while(!c.isAllCardsReady() && timer.elapsed()<2000) cards.pump(n==5);
        if(!require(c.isAllCardsReady(),"ready packets not processed")) return false;
        QByteArray expected=QByteArray::fromHex("fafafafa02000000fa000fa000007d");
        expected.append(43,'\0');
        if(!require(c.buildConfigPacket(16000,500,1000)==expected,"58 byte wire protocol changed")) return false;
        if(!require(c.sendConfigCommand(16000,500,1000,"test"),"configuration rejected")) return false;
        timer.restart();
        while(c.m_configPhase==NetworkController::ConfigPhase::WaitingAck && timer.elapsed()<6000) cards.pump(n==5);
        const auto wanted=n==5 ? NetworkController::ConfigPhase::Failed : NetworkController::ConfigPhase::Confirmed;
        if(!require(c.m_configPhase==wanted,"configuration outcome incorrect")) return false;
        for(int i=0;i<4;++i)
            if(!require(c.m_configAck[i] && c.cardDiagnosticState(i)=="confirmed","confirmed card marked failed")) return false;
        if(n==5 && !require(cards.configs[4]==4 && c.cardDiagnosticState(4)=="failed","fifth-card retry/diagnostic incorrect")) return false;
        Cards::feedback(cards.sockets[0],60); // late feedback after completion
        SOCKET unknown=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        sockaddr_in source{}; source.sin_family=AF_INET;
        inet_pton(AF_INET,"127.0.0.99",&source.sin_addr);
        bind(unknown,reinterpret_cast<sockaddr*>(&source),sizeof(source));
        Cards::feedback(unknown,60); closesocket(unknown);
        timer.restart(); while(timer.elapsed()<60) cards.pump(n==5);
        QTextStream(stdout) << "PASS " << n << " targets: " << (n==5?"fifth timeout; first four confirmed":"all confirmed") << Qt::endl;
        return true;
    }

    static bool pendingSessionCleanup()
    {
        NetworkController c;
        c.m_targetIPs = {QStringLiteral("127.0.0.1")};
        c.m_cardsReady = {true};
        c.m_configPhase = NetworkController::ConfigPhase::Failed;
        c.m_pendingMeasurementSessionId = QStringLiteral("pending-direct");
        c.m_cmdQueue.push_back({NetworkController::PendingCmdType::StartMeasure,
                                0, 0, 0, QString(), QStringLiteral("api"),
                                QStringLiteral("pending-queued")});
        c.retryPendingCommand();
        bool ok = require(c.m_cmdQueue.empty() && c.m_pendingMeasurementSessionId.isEmpty(),
                          "queued config failure clears pending measurement session");

        c.m_targetIPs = {QStringLiteral("127.0.0.1")};
        c.m_cardsReady = {true};
        c.m_configPhase = NetworkController::ConfigPhase::Failed;
        if (!require(c.initControlSocket(), "pending cleanup control socket")) return false;
        const bool accepted = c.sendStartMeasure();
        ok = require(!accepted && c.m_pendingMeasurementSessionId.isEmpty(),
                      "direct config failure clears pending measurement session") && ok;
        c.cleanupControlSocket();
        return ok;
    }

    static bool measurementBoundaryEvidence()
    {
        NetworkController controller;
        AcqConfig config;
        config.nCards = 1;
        config.acqTimeNs = 16000;
        config.localBindIP = "127.0.0.1";
        config.targetIPs = {"127.0.0.1"};
        controller.setDiagnosticContext(QStringLiteral("session-boundary"),
                                        QStringLiteral("test_loopback"));
        if (!require(controller.start(config), "session boundary controller start")) return false;
        controller.m_cardsReady = {true};
        controller.m_configAck = {true};
        controller.m_configPhase = NetworkController::ConfigPhase::Confirmed;
        controller.m_currentConfigId = QStringLiteral("boundary-config");
        if (!require(controller.sendStartMeasure(), "session boundary start transaction")) {
            controller.stop();
            return false;
        }
        const QString sessionId = controller.m_measurementSessionId;
        const bool stopped = controller.sendStopMeasure();
        controller.stop();
        if (!require(stopped, "session boundary stop transaction")) return false;

        DiagnosticRecorder *recorder = DiagnosticRecorder::instance();
        if (!require(recorder && recorder->flush(2000), "session boundary recorder flush")) return false;
        QFile events(QDir(recorder->runDirectory()).filePath(QStringLiteral("events.jsonl")));
        QFile settings(QDir(recorder->runDirectory()).filePath(QStringLiteral("settings_history.jsonl")));
        QFile network(QDir(recorder->runDirectory()).filePath(QStringLiteral("network_history.jsonl")));
        QFile cards(QDir(recorder->runDirectory()).filePath(QStringLiteral("card_history.jsonl")));
        if (!require(events.open(QIODevice::ReadOnly) && settings.open(QIODevice::ReadOnly)
                         && network.open(QIODevice::ReadOnly) && cards.open(QIODevice::ReadOnly),
                     "session boundary evidence files")) return false;

        QHash<QString, int> eventCounts;
        QJsonObject startFenceFields;
        while (!events.atEnd()) {
            const QJsonObject object = QJsonDocument::fromJson(events.readLine()).object();
            const QJsonObject fields = object.value(QStringLiteral("fields")).toObject();
            if (fields.value(QStringLiteral("measurementSessionId")).toString() != sessionId)
                continue;
            const QString message = object.value(QStringLiteral("message")).toString();
            ++eventCounts[message];
            if (message == QStringLiteral("measurement_start_fence"))
                startFenceFields = fields;
        }
        bool ok = true;
        for (const QString& marker : {QStringLiteral("measurement_session_prepare"),
                                       QStringLiteral("measurement_session_armed"),
                                       QStringLiteral("measurement_start_command"),
                                       QStringLiteral("measurement_started"),
                                       QStringLiteral("measurement_stop_command"),
                                       QStringLiteral("measurement_stopped")}) {
            ok = require(eventCounts.value(marker) == 1,
                         QStringLiteral("canonical marker %1 occurs once").arg(marker)) && ok;
        }
        ok = require(eventCounts.value(QStringLiteral("measurement_start_fence")) == 1
                         && startFenceFields.value(QStringLiteral("cardIndex")).toInt() == 0
                         && startFenceFields.value(QStringLiteral("startFenceCommitted")).toBool(),
                     "per-card start fence evidence is committed") && ok;
        const QByteArray sessionBytes = sessionId.toUtf8();
        const QByteArray settingsBytes = settings.readAll();
        const QByteArray networkBytes = network.readAll();
        const QByteArray cardBytes = cards.readAll();
        ok = require(settingsBytes.count(sessionBytes) >= 4,
                      "start/stop settings snapshots carry session id") && ok;
        ok = require(networkBytes.contains(sessionBytes) && cardBytes.contains(sessionBytes),
                      "stop boundary has ingress and card snapshots") && ok;
        return ok;
    }

    static bool measurementBoundaryFailureEvidence()
    {
        NetworkController controller;
        AcqConfig config;
        config.nCards = 1;
        config.acqTimeNs = 16000;
        config.localBindIP = "127.0.0.1";
        config.targetIPs = {"127.0.0.1"};
        controller.setDiagnosticContext(QStringLiteral("session-boundary-failure"),
                                        QStringLiteral("test_loopback"));
        if (!require(controller.start(config), "failure evidence controller start")) return false;
        controller.m_cardsReady = {true};
        controller.m_configAck = {true};
        controller.m_configPhase = NetworkController::ConfigPhase::Confirmed;
        controller.m_currentConfigId = QStringLiteral("boundary-failure-config");

        // Stop the actual receiver thread before Start.  This injects a
        // receiver prepare/disarm failure through the production transaction,
        // without a fake dispatch path or hardware protocol change.
        controller.m_receivers.front()->requestStop();
        controller.m_receivers.front()->wait(2000);
        const bool started = controller.sendStartMeasure();
        const QString sessionId = controller.m_measurementSessionId;
        bool ok = require(!started && controller.m_measurementState
                              == NetworkController::MeasurementState::Fault,
                          "receiver barrier failure blocks start and enters fault")
                  && require(controller.m_pendingMeasurementSessionId.isEmpty(),
                              "receiver barrier failure clears pending session")
                  && require(!sessionId.isEmpty(), "failed session id retained for diagnosis");

        DiagnosticRecorder *recorder = DiagnosticRecorder::instance();
        ok = require(recorder && recorder->flush(2000), "failure evidence recorder flush") && ok;
        QFile events(QDir(recorder->runDirectory()).filePath(QStringLiteral("events.jsonl")));
        QFile settings(QDir(recorder->runDirectory()).filePath(QStringLiteral("settings_history.jsonl")));
        if (!require(events.open(QIODevice::ReadOnly) && settings.open(QIODevice::ReadOnly),
                     "failure evidence files")) {
            controller.stop();
            return false;
        }
        QHash<QString, int> eventCounts;
        while (!events.atEnd()) {
            const QJsonObject object = QJsonDocument::fromJson(events.readLine()).object();
            const QJsonObject fields = object.value(QStringLiteral("fields")).toObject();
            if (fields.value(QStringLiteral("measurementSessionId")).toString() == sessionId
                && fields.value(QStringLiteral("configId")).toString()
                       == QStringLiteral("boundary-failure-config"))
                ++eventCounts[object.value(QStringLiteral("message")).toString()];
        }
        ok = require(eventCounts.value(QStringLiteral("measurement_start_failed")) == 1,
                     "failed start canonical marker occurs once") && ok;
        ok = require(eventCounts.value(QStringLiteral("measurement_start_rollback")) == 1,
                     "rollback canonical marker occurs once") && ok;
        ok = require(eventCounts.value(QStringLiteral("measurement_started")) == 0,
                     "failed start has no started marker") && ok;
        const QByteArray settingsEvidence = settings.readAll();
        ok = require(settingsEvidence.contains(sessionId.toUtf8())
                         && settingsEvidence.contains("measurement_start_rollback"),
                     "rollback settings snapshot carries session boundary") && ok;
        controller.stop();
        return ok;
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc,argv);
    const bool legacyControlOnly=app.arguments().contains("--legacy-control-only");
    WSADATA winsock{};
    if(WSAStartup(MAKEWORD(2,2),&winsock)!=0) return 2;
    QTemporaryDir temporary;
    DiagnosticRecorder::Options options;
    options.rootDirectory=temporary.path(); options.noiseBurst=0;
    auto *recorder=DiagnosticRecorder::initialize(options);
    const bool ok=runtimeStatsFieldMapping()
        && roundPolicyPropagation()
        && NetworkDiagnosticTestAccess::exercise(4) && NetworkDiagnosticTestAccess::exercise(5)
        && NetworkDiagnosticTestAccess::pendingSessionCleanup()
        && (legacyControlOnly || (NetworkDiagnosticTestAccess::measurementBoundaryEvidence()
        && NetworkDiagnosticTestAccess::measurementBoundaryFailureEvidence()
        && receptionDuringExport(false,QString())
        && receptionDuringExport(true,QDir(temporary.path()).filePath("during-reception.zip"))));
    QThreadPool::globalInstance()->waitForDone();
    const auto result=recorder->exportRun(QDir(temporary.path()).filePath("control-test.zip"));
    QFile evidence(QDir(recorder->runDirectory()).filePath("events.jsonl"));
    const bool readable=evidence.open(QIODevice::ReadOnly);
    const QByteArray events=evidence.readAll();
    const bool evidenceOk=require(readable && events.contains("config_failed")
        && events.contains("config_confirmed") && events.contains("test-5-cards")
        && events.contains("127.0.0.5") && events.contains("retry_reset"),
        "configuration evidence was not persisted");
    evidence.close();
    DiagnosticRecorder::shutdown();
    WSACleanup();
    return ok && result.success && evidenceOk ? 0 : 1;
}
