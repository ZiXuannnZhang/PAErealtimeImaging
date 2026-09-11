#include "NetworkController.h"
#include "StartupPolicy.h"
#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QUuid>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <QSysInfo>

namespace {
QJsonObject processorTopology(){
    DWORD bytes=0;GetLogicalProcessorInformationEx(RelationProcessorCore,nullptr,&bytes);
    std::vector<unsigned char> storage(bytes);
    if(!bytes||!GetLogicalProcessorInformationEx(RelationProcessorCore,
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data()),&bytes))
        return {{"status","unknown"},{"error",int(GetLastError())}};
    QJsonArray cores;
    for(DWORD offset=0;offset<bytes;){
        const auto info=reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(storage.data()+offset);
        if(!info->Size||offset+info->Size>bytes)return {{"status","unknown"},{"error","invalid topology record"}};
        QJsonArray groups;
        for(WORD g=0;g<info->Processor.GroupCount;++g)groups.append(QJsonObject{
            {"group",info->Processor.GroupMask[g].Group},
            {"logicalMaskHex",QString::number(quint64(info->Processor.GroupMask[g].Mask),16)}});
        cores.append(QJsonObject{{"coreIndex",cores.size()},{"groups",groups}});offset+=info->Size;
    }
    return {{"status","known"},{"cores",cores}};
}
QJsonArray interfaceIdentity(const std::string& localIp){
    QJsonArray result;ULONG bytes=16*1024;std::vector<unsigned char> storage(bytes);ULONG status=ERROR_BUFFER_OVERFLOW;
    for(int attempt=0;attempt<3&&status==ERROR_BUFFER_OVERFLOW;++attempt){storage.resize(bytes);status=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_PREFIX,nullptr,reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data()),&bytes);}
    if(status!=NO_ERROR){result.append(QJsonObject{{"status","unknown"},{"api","GetAdaptersAddresses"},{"error",int(status)}});return result;}
    for(auto adapter=reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data());adapter;adapter=adapter->Next){bool match=localIp.empty();QJsonArray addresses;for(auto u=adapter->FirstUnicastAddress;u;u=u->Next){char text[INET_ADDRSTRLEN]{};auto in=reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);InetNtopA(AF_INET,&in->sin_addr,text,sizeof(text));addresses.append(text);if(localIp==text)match=true;}if(!match)continue;
        QJsonObject o{{"status","known"},{"interfaceIndex",int(adapter->IfIndex)},{"interfaceLuid",QString::number(quint64(adapter->Luid.Value))},{"adapterGuid",QString::fromLatin1(adapter->AdapterName?adapter->AdapterName:"")},{"friendlyName",adapter->FriendlyName?QString::fromWCharArray(adapter->FriendlyName):QString("unknown")},{"description",adapter->Description?QString::fromWCharArray(adapter->Description):QString("unknown")},{"receiveLinkSpeed",QString::number(quint64(adapter->ReceiveLinkSpeed))},{"transmitLinkSpeed",QString::number(quint64(adapter->TransmitLinkSpeed))},{"localIPv4",addresses}};result.append(o);}
    if(result.isEmpty())result.append(QJsonObject{{"status","unknown"},{"error","bound address did not map to an adapter"}});return result;
}
}

bool NetworkController::createPaimageBackend(QString& error){
    paimage::Backend::Settings settings;
    // Single configuration source: AcqConfig.startupIdleMs decides the
    // startup admission policy actually passed to the receiver; the identity
    // below records both the requested policy and the effective value.
    settings.acquisition={m_config.nCards,m_config.samplesPerTrig(),m_config.bitsPerChannel,m_config.startupIdleMs};
    settings.localIp=m_config.localBindIP;
    for(const auto& ip:m_targetIPs)settings.targets.push_back(ip.toStdString());
    std::vector<DataProcessor*> processors;std::vector<FileSaver*> savers;
    for(auto& p:m_processors)processors.push_back(p.get());
    for(auto& s:m_savers)savers.push_back(s.get());
    try{
        const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString tracePath=QDir(QCoreApplication::applicationDirPath()).filePath("paimage-traces/"+id);
        if(!QDir().mkpath(tracePath))throw std::runtime_error("trace directory creation failed");
        if(m_config.diagnosticTraceEnabled)
            m_paimageTrace=std::make_unique<paimage::TraceWriter>(std::filesystem::path(tracePath.toStdWString()));
        if(m_config.diagnosticLevel>=1)
            m_paimageTiming=std::make_unique<paimage::TimingWriter>(std::filesystem::path(tracePath.toStdWString()));
        if(m_config.diagnosticLevel>=1)
            m_paimageLoopLog=std::make_unique<paimage::LoopLog>(std::filesystem::path(tracePath.toStdWString()));
        QJsonArray targets;for(const auto& ip:m_targetIPs)targets.append(ip);
        QJsonObject identity{{"runId",id},{"backendId","paimage-receiver-diagnostics"},{"schemaVersion",3},
            {"wallAnchorMs",double(QDateTime::currentMSecsSinceEpoch())},{"monotonicAnchorNs",QString::number(paimage::SocketReceiver::now())},
            {"cpuTopology",processorTopology()},{"affinityRequested",false},{"diagnosticTraceEnabled",m_config.diagnosticTraceEnabled},
            {"diagnosticLevel",m_config.diagnosticLevel},{"timingEnabled",m_config.diagnosticLevel>=1},
            {"diagnosticModes",QJsonObject{{"0","raw-ingress only"},{"1","raw-ingress plus lightweight timing"},{"2","lightweight timing plus externally managed system capture index"}}},
            {"listenId",m_diagnosticListenId},{"configId",m_currentConfigId},{"samples",m_config.samplesPerTrig()},
            {"bits",m_config.bitsPerChannel},{"cards",m_config.nCards},{"dataPort",8001},{"feedbackPort",8000},
            {"targets",targets},{"startupPolicy",m_config.startupIdleMs>0?"legacy":"bypass"},
            {"startupIdleMsRequested",m_config.startupIdleMs>0?1000:0},
            {"startupIdleMsEffective",m_config.startupIdleMs},
            {"localBindIP",QString::fromStdString(m_config.localBindIP)},
            {"interfaces",interfaceIdentity(m_config.localBindIP)},{"osVersion",QSysInfo::prettyProductName()},
            {"processId",double(QCoreApplication::applicationPid())},{"receiverThreadIdStatus","recorded in timing thread-life records"},
            {"driverProviderVersionStatus","unknown unless Windows adapter APIs expose it; system capture manifest retains tool output"}};
        QFile metadata(QDir(tracePath).filePath("run-config.json"));
        if(!metadata.open(QIODevice::WriteOnly)||metadata.write(QJsonDocument(identity).toJson())<0)throw std::runtime_error("trace identity write failed");
        metadata.close();
        m_paimage=std::make_unique<paimage::Backend>(settings,processors,savers,m_paimageTrace.get(),m_paimageTiming.get(),m_paimageLoopLog.get());
        m_paimageRunId=id;
        const int expected=m_config.packetsPerTrig();
        m_paimage->receiver().ingressSink=[this](int card,const paimage::TraceRecord& r){
            if(card>=0&&card<int(m_processors.size())){auto& stats=m_processors[card]->stats();
                ++stats.packetsReceived;++stats.socketPacketsReceived;stats.socketBytesReceived.fetch_add(r.length);
                if(r.length>=4)stats.observeRawReceive(r.trigger,r.packet);
            }
        };
        m_paimage->receiver().observationSink=[this,expected](const paimage::Observation& o){
            if(o.card<0||o.card>=int(m_processors.size()))return;
            auto& stats=m_processors[o.card]->stats();
            if(o.decision==paimage::Decision::Disabled)++stats.sessionBoundaryPacketsDiscarded;
            if(o.decision==paimage::Decision::Duplicate)++stats.assemblyDuplicatePackets;
            if(o.decision==paimage::Decision::OffsetOutside)++stats.assemblyOffsetOutOfRangePackets;
            if(o.decision==paimage::Decision::RecentTrigger)++stats.staleTriggerPacketsDiscarded;
            if(o.decision==paimage::Decision::Complete)++stats.triggersComplete;
            else if(o.decision==paimage::Decision::TriggerSwitch||o.decision==paimage::Decision::Timeout){
                ++stats.triggersPartial;stats.packetsDropped.fetch_add(expected>int(o.count)?expected-o.count:0);
            }
        };
        const auto generation=++m_paimageGeneration;
        m_paimage->feedbackSink=[this,generation](int card,int type,qint64 receivedNs){
            QMetaObject::invokeMethod(this,[this,generation,card,type,receivedNs]{
                if(!m_running||!m_paimage||m_paimageGeneration!=generation)return;
                const auto dispatchBegin=paimage::SocketReceiver::now();
                m_paimage->feedback(card,type);
                const auto dispatchEnd=paimage::SocketReceiver::now();
                if(type==1){m_cardsReady.at(card)=true;emit cardReady(card);if(isAllCardsReady())emit allCardsReady();}
                if(type==2)emit configAcked(card);
                recordDiagnosticEvent("paimage.control","feedback_dispatched",
                    DiagnosticRecorder::Severity::Info,
                    {{"card",card},{"feedbackType",type},{"receiveMonotonicNs",QString::number(receivedNs)},
                     {"dispatchStartNs",QString::number(dispatchBegin)},
                     {"dispatchEndNs",QString::number(dispatchEnd)},
                     {"uiCallbackSpanNs",QString::number(dispatchEnd-dispatchBegin)},
                     {"uiDispatchLatencyNs",QString::number(dispatchBegin>=receivedNs?dispatchBegin-receivedNs:0)}});
                pollPaimage();
            },Qt::QueuedConnection);
        };
        std::string message;if(!m_paimage->listen(message)){error=QString::fromStdString(message);m_paimage.reset();return false;}
        recordDiagnosticEvent("paimage.lifecycle","source_listener_created",DiagnosticRecorder::Severity::Info,
            {{"traceDirectory",tracePath},{"runId",id},{"samples",m_config.samplesPerTrig()}});
        scheduleNetworkSnapshot("paimage_listener_created","listening",m_currentConfigId);
        return true;
    }catch(const std::exception& e){error=QString::fromUtf8(e.what());m_paimage.reset();return false;}
}

bool NetworkController::startPaimage(const AcqConfig& config,std::function<void()> onStarted,std::function<void()> onFailed){
    if(config.nCards<1||config.nCards>32||config.samplesPerTrig()<128||
       (config.bitsPerChannel!=16&&config.bitsPerChannel!=32)){
        emit errorOccurred(QStringLiteral("PAimage采集配置无效：检查卡数、位宽及采样点数"));if(onFailed)onFailed();return false;}
    if(m_running||m_stopThread.joinable()){emit errorOccurred(QStringLiteral("请等待当前监听停止后再启动"));if(onFailed)onFailed();return false;}
    m_config=config;m_targetIPs.clear();
    m_paimageLastBytes.assign(config.nCards,0);
    for(int c=0;c<config.nCards;++c)m_targetIPs.push_back(config.targetIPs.empty()?QString("192.168.0.%1").arg(c+2):
        (c<int(config.targetIPs.size())?QString::fromStdString(config.targetIPs[c]):QString()));
    if(m_config.localBindIP.empty()&&!m_targetIPs.empty()){
        // Host interface selection only: read Windows route, then explicitly
        // bind the recovered control socket. No interface/ARP/driver mutation.
        SOCKADDR_INET destination{},source{};destination.si_family=AF_INET;
        InetPtonA(AF_INET,m_targetIPs.front().toStdString().c_str(),&destination.Ipv4.sin_addr);
        MIB_IPFORWARD_ROW2 route{};
        if(GetBestRoute2(nullptr,0,nullptr,&destination,0,&route,&source)==NO_ERROR){
            char text[INET_ADDRSTRLEN]{};InetNtopA(AF_INET,&source.Ipv4.sin_addr,text,sizeof(text));m_config.localBindIP=text;
        }
    }
    m_cardsReady.assign(config.nCards,false);m_cardDiagnostics.assign(config.nCards,CardDiagnosticState{});
    m_lastPktsReceived.assign(config.nCards,0);m_lastPktsDropped.assign(config.nCards,0);m_lastTrigsComplete.assign(config.nCards,0);
    m_measurementSessionToken=0;m_measurementRunning=false;m_paimageStartPending=false;
    m_measurementState=MeasurementState::Disarmed;m_configPhase=ConfigPhase::Idle;
    m_processors.clear();m_savers.clear();m_displayBuffers.clear();
    m_publisher=std::make_unique<FramePublisher>(this);m_publisher->configure(config.enablePublisher,config.nCards,config.samplesPerTrig());
    if(config.enablePublisher)m_publisher->start();
    for(int c=0;c<config.nCards;++c){
        auto display=std::make_unique<DisplayBuffer>();auto saver=std::make_unique<FileSaver>(c);
        saver->setSessionDirResolver([this](auto gen){return sessionDir(gen);});
        auto processor=std::make_unique<DataProcessor>(c,nullptr,display.get(),config.enablePublisher?m_publisher.get():nullptr,config,m_ringFeedSink);
        processor->setSessionGenReader([this]{return autoSessionGen();});
        connect(saver.get(),&FileSaver::errorOccurred,this,&NetworkController::errorOccurred);
        connect(saver.get(),&FileSaver::statusMessage,this,&NetworkController::statusMessage);
        m_displayBuffers.push_back(std::move(display));m_savers.push_back(std::move(saver));m_processors.push_back(std::move(processor));
    }
    QString error;if(!createPaimageBackend(error)){
        if(m_publisher){m_publisher->requestInterruption();m_publisher->wait();m_publisher.reset();}
        if(m_paimageTrace)m_paimageTrace->stop();
        if(m_paimageTiming)m_paimageTiming->stop();
        if(m_paimageLoopLog)m_paimageLoopLog->stop();
        m_paimage.reset();m_paimageTrace.reset();m_paimageTiming.reset();m_paimageLoopLog.reset();
        emit errorOccurred("PAimage-derived 监听失败："+error);if(onFailed)onFailed();return false;
    }
    m_running=true;m_lastStatsMs=QDateTime::currentMSecsSinceEpoch();m_lastRuntimeSnapshotMs=0;m_lastIngressSnapshotMs=0;
    m_paimageTimer=new QTimer(this);connect(m_paimageTimer,&QTimer::timeout,this,&NetworkController::pollPaimage);m_paimageTimer->start(20);
    m_paimageLoopMonitorTimer=new QTimer(this);connect(m_paimageLoopMonitorTimer,&QTimer::timeout,this,&NetworkController::pollPaimageLoopMonitor);m_paimageLoopMonitorTimer->start(100);
    m_paimageLastBurstEpoch=0;m_paimageLastStallWarnMs=0;
    m_statsTimer=new QTimer(this);connect(m_statsTimer,&QTimer::timeout,this,&NetworkController::onStatsTimer);m_statsTimer->start(STATS_UPDATE_MS);
    recordDiagnosticEvent("paimage.lifecycle","listener_started",DiagnosticRecorder::Severity::Info,
        {{"backendId","paimage-derived"},{"localBindIP",QString::fromStdString(m_config.localBindIP)},
         {"startupPolicy",m_config.startupIdleMs>0?"legacy":"bypass"},
         {"startupIdleMsRequested",m_config.startupIdleMs>0?1000:0},
         {"startupIdleMsEffective",m_config.startupIdleMs},
         {"startupTrialId",QString::fromStdString(startupTrialId())}});
    emit statusMessage(QStringLiteral("PAimage-derived：单接收线程及两个源输出线程已启动"));if(onStarted)onStarted();return true;
}

void NetworkController::recordPaimageSnapshot(){
    if(!m_paimage)return;
    const auto counters=m_paimage->receiver().counters();QJsonArray buffers,bytes;
    const auto outputStats=m_paimage->output().stats();
    for(auto n:m_paimage->receiver().receiveBuffers())buffers.append(n<0?QJsonValue("unknown"):QJsonValue(n));
    for(const auto& p:m_processors)bytes.append(QString::number(p->stats().socketBytesReceived.load()));
    QJsonObject fields{{"backendId","paimage-derived"},{"configId",m_currentConfigId},
        {"measurementSessionId",m_measurementSessionId},{"traceSession",QString::number(m_measurementSessionToken)},
        {"recvfromDatagrams",QString::number(m_paimage->receiver().ingress())},{"perCardDataBytes",bytes},
        {"sourceCompleteCards",QString::number(counters.completeCards)},
        {"sourceStartupFilteredCards",QString::number(counters.startupFilteredCards)},
        {"sourceStartupFilteredSync",QString::number(counters.startupFilteredSync)},
        {"sourceStartupIncompleteCounter",QString::number(counters.startupIncomplete)},
        {"sourceRuntimeIncompleteCounter",QString::number(counters.runtimeIncomplete)},
        {"incompleteCounterUnit","source mixed card/sync events; inspect per-object trace"},
        {"dataReceiveBuffers",buffers},{"feedbackReceiveBuffer",m_paimage->receiver().feedbackReceiveBuffer()},
        {"receivePriorityRequested",2},{"receivePriorityError",m_paimage->receiver().priorityResult()},
        {"receivePriorityActual",m_paimage->receiver().actualPriority()},
        {"receiveHardErrors",QString::number(m_paimage->receiver().hardErrors())},
        {"lastSocketError",m_paimage->receiver().lastSocketError()},
        {"affinityQuery","unknown; no affinity request"},{"driverVersion","unknown"},
        {"sourceSyncBlockSize",50},
        {"savingRequested",m_paimageSavingRequested},{"legacyProcessorCountersNotApplicable",true},
        {"saveQueueEnqueued",QString::number(outputStats.saveEnqueued)},
        {"saveQueueDequeued",QString::number(outputStats.saveDequeued)},
        {"saveQueueCurrentDepth",QString::number(outputStats.saveCurrentDepth)},
        {"saveQueuePeakDepth",QString::number(outputStats.savePeakDepth)},
        {"saveQueueFull",QString::number(outputStats.saveQueueFull)},
        {"saveWorkerMaxNs",QString::number(outputStats.maxSaveWorkerNs)},
        {"traceIncomplete",m_paimageTrace&&m_paimageTrace->incomplete()}};
    recordDiagnosticEvent("paimage.snapshot","source_snapshot",DiagnosticRecorder::Severity::Info,fields);
}

void NetworkController::pollPaimageLoopMonitor(){
    if(!m_running||!m_paimage||!m_paimageLoopLog)return;
    const auto nowNs=paimage::SocketReceiver::now();
    const auto progress=m_paimageLoopLog->lastProgressNs();
    // 100 ms sampling cannot observe every 20 ms stall and this hint alone
    // never proves CPU preemption; WPR scheduling evidence must match.
    if(m_paimage->receiver().isRunning()&&progress&&nowNs-progress>20*1000000){
        paimage::LoopRecord stalled;stalled.timeNs=nowNs;stalled.spanNs=nowNs-progress;
        stalled.kind=std::uint16_t(paimage::LoopKind::Stalled);stalled.threadId=quint32(quintptr(QThread::currentThreadId()));
        stalled.payloadA=progress;m_paimageLoopLog->push(stalled);
        const auto nowMs=QDateTime::currentMSecsSinceEpoch();
        if(nowMs-m_paimageLastStallWarnMs>=5000){m_paimageLastStallWarnMs=nowMs;
            recordDiagnosticEvent("paimage.loop","loop_stall_hint",DiagnosticRecorder::Severity::Warning,
                {{"progressMonotonicNs",QString::number(progress)},
                 {"stalledNs",QString::number(nowNs-progress)},
                 {"samplingIntervalMs",100},
                 {"note","100ms sampler cannot observe every 20ms stall and does not prove CPU preemption"}});}
    }
    const auto epoch=m_paimageLoopLog->freezeEpoch();
    if(epoch!=m_paimageLastBurstEpoch){
        m_paimageLastBurstEpoch=epoch;
        recordDiagnosticEvent("paimage.loop","receive_burst_detected",DiagnosticRecorder::Severity::Info,
            {{"freezeEpoch",QString::number(epoch)},{"trialId",QString::fromStdString(startupTrialId())},
             {"runId",m_paimageRunId},{"detectMonotonicNs",QString::number(nowNs)},
             {"note","burst marker only; session handling, assembly cleanup and round attribution are unchanged"}});
        // The notification is emitted 10 s after the burst so a system
        // capture keeps covering the burst itself. The receive thread only
        // writes its loop stream and never waits for any script response.
        QTimer::singleShot(10000,this,[this,epoch,nowNs]{
            if(!m_paimageLoopLog||m_paimageLoopLog->freezeEpoch()<epoch)return;
            writeSystemCaptureNotification(epoch,nowNs);
        });
    }
    if(m_paimageSaveGeneration&&!m_paimageSaveAppliedLogged&&
       m_paimage->output().savingApplied(m_paimageSaveGeneration)){
        m_paimageSaveAppliedLogged=true;
        recordDiagnosticEvent("paimage.save","save_configuration_applied",DiagnosticRecorder::Severity::Info,
            {{"generation",QString::number(m_paimageSaveGeneration)},
             {"appliedMonotonicNs",QString::number(nowNs)},
             {"directory",m_paimageSaveDir}});
    }
}

void NetworkController::writeSystemCaptureNotification(quint64 epoch, qint64 burstNs){
    const QString channel=QString::fromStdString(systemCaptureChannelDir());
    const QString trial=QString::fromStdString(startupTrialId());
    QDir().mkpath(channel);
    const QJsonObject note{{"kind","burst"},{"trialId",trial},{"runId",m_paimageRunId},
        {"freezeEpoch",QString::number(epoch)},{"burstMonotonicNs",QString::number(burstNs)},
        {"notifiedMonotonicNs",QString::number(paimage::SocketReceiver::now())},
        {"notifiedWallMs",QDateTime::currentMSecsSinceEpoch()}};
    const QString path=QDir(channel).filePath(QString("burst-%1-%2.json").arg(qulonglong(epoch)).arg(m_paimageRunId));
    QFile file(path);
    const bool wrote=file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&
        file.write(QJsonDocument(note).toJson(QJsonDocument::Compact))>=0;
    if(wrote)file.close();
    recordDiagnosticEvent("paimage.loop",wrote?"burst_notification_written":"burst_notification_write_failed",
        DiagnosticRecorder::Severity::Info,
        {{"freezeEpoch",QString::number(epoch)},{"trialId",trial},{"runId",m_paimageRunId},
         {"path",path},{"written",wrote}});
}

void NetworkController::stopPaimage(){
    if(!m_running)return;recordPaimageSnapshot();m_running=false;m_paimageStartPending=false;
    if(m_paimageTimer){m_paimageTimer->stop();m_paimageTimer->deleteLater();m_paimageTimer=nullptr;}
    if(m_paimageLoopMonitorTimer){m_paimageLoopMonitorTimer->stop();m_paimageLoopMonitorTimer->deleteLater();m_paimageLoopMonitorTimer=nullptr;}
    if(m_statsTimer){m_statsTimer->stop();m_statsTimer->deleteLater();m_statsTimer=nullptr;}
    if(m_paimage)m_paimage->requestStop();if(m_publisher)m_publisher->requestInterruption();
    m_stopThread=std::thread([this]{
        if(m_paimage)m_paimage->stop();if(m_publisher)m_publisher->wait();if(m_paimageTrace)m_paimageTrace->stop();if(m_paimageTiming)m_paimageTiming->stop();
        if(m_paimageLoopLog)m_paimageLoopLog->stop();
        QMetaObject::invokeMethod(this,[this]{
            if(m_stopThread.joinable())m_stopThread.join();
            m_paimage.reset();m_paimageTrace.reset();m_paimageTiming.reset();m_paimageLoopLog.reset();m_processors.clear();m_savers.clear();m_displayBuffers.clear();m_publisher.reset();
            m_measurementRunning=false;m_paimageSavingRequested=false;emit stopped();
        },Qt::QueuedConnection);
    });
}

bool NetworkController::configurePaimage(int ns,int a,int b,const QString& trigger){
    if(!m_running||m_measurementRunning)return false;
    m_currentConfigId=QString("config-%1").arg(m_nextConfigId++);
    if(ns!=m_config.acqTimeNs){
        // Source 141c70 copies configuration only while listener is stopped.
        // Preserve host workflow by rebuilding that immutable source listener.
        if(m_paimageSavingRequested)m_paimage->output().prepareConfigurationRestart();
        m_paimage->stop();m_paimage.reset();
        if(m_paimageTrace)m_paimageTrace->stop();m_paimageTrace.reset();
        if(m_paimageTiming)m_paimageTiming->stop();m_paimageTiming.reset();
        if(m_paimageLoopLog)m_paimageLoopLog->stop();m_paimageLoopLog.reset();
        m_config.acqTimeNs=ns;
        QString error;if(!createPaimageBackend(error)){emit errorOccurred(error);stopPaimage();return false;}
        if(m_paimageSavingRequested)m_paimage->output().resumeSaving();
        recordDiagnosticEvent("paimage.lifecycle","configuration_listener_restart",DiagnosticRecorder::Severity::Info,{{"samples",m_config.samplesPerTrig()}});
    }
    m_config.delayA=a;m_config.delayB=b;
    recordDiagnosticEvent("paimage.config","configuration_requested",DiagnosticRecorder::Severity::Info,
        {{"configId",m_currentConfigId},{"acqTimeNs",ns},{"delayA",a},{"delayB",b},
         {"samples",m_config.samplesPerTrig()},{"bits",m_config.bitsPerChannel},{"sourceSyncBlockSize",50}});
    m_currentConfigTrigger=trigger;m_paimageConfigReported=false;m_configPhase=ConfigPhase::WaitingAck;
    const bool ok=m_paimage->configure(ns,a,b);pollPaimage();return ok;
}
void NetworkController::pollPaimage(){
    if(!m_running||!m_paimage)return;
    if(!m_paimage->receiver().isRunning()){
        emit errorOccurred(QString("PAimage接收线程已退出，socket错误码 %1").arg(m_paimage->receiver().lastSocketError()));
        stopPaimage();return;
    }
    m_paimage->poll();
    if(!m_paimageConfigReported&&!m_paimage->configuring()&&m_configPhase==ConfigPhase::WaitingAck){
        m_paimageConfigReported=true;
        if(m_paimage->configured()){m_configPhase=ConfigPhase::Confirmed;emit configConfirmed();}
        else{m_configPhase=ConfigPhase::Failed;for(int c=0;c<m_config.nCards;++c)if(!m_paimage->acknowledgements()[c])emit configAckFailed(c);
            if(m_paimageStartPending){m_paimageStartPending=false;emit measurementStartFailed(m_pendingMeasurementSessionId,"PAimage CONFIG failed");}}
    }
    if(m_paimageStartPending&&m_paimage->configured()){m_paimageStartPending=false;startPaimageMeasurement();}
}
bool NetworkController::startPaimageMeasurement(){
    if(!m_running||m_measurementRunning)return false;
    if(m_paimage->configuring()){m_paimageStartPending=true;m_pendingMeasurementSessionId=newMeasurementSessionId();return true;}
    const QString id=m_pendingMeasurementSessionId.isEmpty()?newMeasurementSessionId():m_pendingMeasurementSessionId;
    m_pendingMeasurementSessionId.clear();
    bool ok=m_paimage->startMeasurement(++m_measurementSessionToken);
    recordDiagnosticEvent("paimage.measure",ok?"measurement_started":"measurement_start_failed",DiagnosticRecorder::Severity::Info,
        {{"measurementSessionId",id},{"traceSession",QString::number(m_measurementSessionToken)},{"configId",m_currentConfigId},{"backendId","paimage-derived"}});
    if(ok){m_measurementSessionId=id;m_measurementRunning=true;m_measurementState=MeasurementState::Running;emit measurementStarted(id);}
    else emit measurementStartFailed(id,"PAimage START failed or CONFIG unconfirmed");return ok;
}
bool NetworkController::stopPaimageMeasurement(){
    m_paimageStartPending=false;m_pendingMeasurementSessionId.clear();
    const bool ok=m_paimage->stopMeasurement();
    recordDiagnosticEvent("paimage.measure",ok?"measurement_stopped":"measurement_stop_failed",DiagnosticRecorder::Severity::Info,
        {{"measurementSessionId",m_measurementSessionId},{"traceSession",QString::number(m_measurementSessionToken)},{"configId",m_currentConfigId},{"commandSucceeded",ok}});
    if(ok){m_measurementRunning=false;m_measurementState=MeasurementState::Disarmed;emit measurementStopped(m_measurementSessionId,true);}
    else emit measurementStopFailed(m_measurementSessionId,"PAimage STOP send failed");return ok;
}
