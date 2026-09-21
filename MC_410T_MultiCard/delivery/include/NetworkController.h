#pragma once
#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <vector>
#include <deque>
#include <memory>
#include <optional>
#include <atomic>
#include <thread>
#include <functional>
#include "DataTypes.h"
#include "AcqConfig.h"
#include "DataProcessor.h"
#include "FileSaver.h"
#include "MeasurementSession.h"
#include "FramePublisher.h"
#include "DisplayBuffer.h"
#include "DiagnosticRecorder.h"
#include "NetworkDiagnostics.h"
#include "PaimageAcquisition/AutoSaveRoundCoordinator.h"

#ifdef _WIN32
    #include <winsock2.h>
    typedef SOCKET SocketType;
#else
    #include <sys/socket.h>
    typedef int SocketType;
    #define INVALID_SOCKET -1
#endif

#include "MultiPortReceiver.h"
#include "PaimageAcquisition/Backend.h"

// ============================================================
// NetworkController  多线程组统一管理（采集系统核心协调者）
//
// start() 根据 AcqConfig 创建全部线程并启动
// stop()  有序停止：先停接收  再停处理  再停存储
// ============================================================
class NetworkController : public QObject {
    Q_OBJECT

public:
    explicit NetworkController(QObject* parent = nullptr);
    ~NetworkController() override;

    //  环形实时馈送回调（必须在 start() 之前设置，转发给每张卡 DataProcessor）
    void setRingFeedSink(const DataProcessor::RingFeedSink& sink) { m_ringFeedSink = sink; }

    // Physical-round boundary notifications are rare control/timeout events.
    // The callback is installed before start and may be invoked by the source
    // output thread; consumers must keep it bounded and thread-safe.
    void setPhysicalRoundBoundarySink(
        const paimage::PhysicalRoundNormalizer::Observer& sink) {
        m_physicalRoundBoundarySink = sink;
    }

    //  主接口（MainWindow 调用）
    // onStarted：初始化完成后的回调（同步调用，可直接操作 UI）
    // onFailed：初始化失败时调用，可为 nullptr
    // 返回 true：所有接收端口绑定成功并进入接收循环；
    // 返回 false：接收端口启动失败（内部已同步回滚，onFailed 同步调用）
    bool start(const AcqConfig& config,
               std::function<void()> onStarted = nullptr,
               std::function<void()> onFailed  = nullptr);

    // stop()：立即发出停止信号并返回（非阻塞）
    // 线程的实际等待/terminate 在后台 std::thread 完成，完成后发出 stopped() 信号
    void stop();

    //  网段扫描：探测 baseIP 起 count 个 IP 的可达性，返回在线 IP 列表
    //  专用网段假设：在线 IP 即采集卡（上位机/交换机避开扫描范围），
    //  用于自动识别采集卡数量与目标 IP。扫描范围由注册表
    //  NetworkParams/ScanBaseIP、ScanIPCount 控制（默认 .2 起 32 个）。
    static QVector<QString> scanReachableIPs(const QString& baseIP,
                                             int count,
                                             const QString& listenId = QString());

    // 设置本次监听的诊断关联信息。该上下文只在控制器线程使用，
    // 反馈接收线程通过不可变的 receiveSequence 与控制器线程关联。
    void setDiagnosticContext(const QString& listenId, const QString& source);

    // 检查所有子线程是否已全部退出（供 closeEvent 异步轮询）
    bool allThreadsStopped() const {
        if (m_stopThread.joinable()) return false;  // 后台停止线程还在运行
        return true;
    }

    //  存储控制
    void startSaving(const QString& directory,
                     int triggersPerFile,
                     const QString& suffix);
    void stopSaving();
    // 自动保存会话代协调器：目录准备/注册完成后才发布 generation。
    // 物理边界源调用 commit，HostOutput 按 round identity 解析后再打标。
    using AutoSaveCommit = paimage::AutoSaveRoundCoordinator::CommitResult;
    void configureAutoSave(const QString& baseDirectory,
                           std::uint64_t lastDirectoryNumber);
    AutoSaveCommit beginAutoSaveSession(
        std::uint64_t measurementSession,
        const QString& phase = QStringLiteral("ui_session_start"));
    // Bind round zero before the first normalized LogicalScan reaches the
    // HostOutput save stamp.  The source path calls the same method at
    // measurement-session start; it does not allocate a new directory.
    AutoSaveCommit bindAutoSaveMeasurementSession(
        std::uint64_t measurementSession,
        const QString& phase = QStringLiteral("source_measurement_session_start"));
    AutoSaveCommit bindAutoSaveMeasurementRound(
        std::uint64_t measurementSession,
        std::uint64_t roundGeneration,
        const QString& phase = QStringLiteral("active_measurement_round_start"));
    AutoSaveCommit commitAutoSaveBoundary(
        std::uint64_t measurementSession,
        std::uint64_t roundGeneration,
        paimage::AutoSaveBoundaryKind boundaryKind,
        const QString& phase);
    void disableAutoSave();
    bool autoSaveEnabled() const;
    bool autoSaveFaulted() const;
    uint64_t autoSessionGen() const;
    QString sessionDir(uint64_t gen) const;
    // Round-aware resolver used by HostOutput before a TriggerGroup enters
    // the save worker.  It returns 0 for manual/disabled save and the
    // coordinator's fail-closed sentinel for an enabled lookup failure.
    uint64_t resolveAutoSaveRound(uint64_t measurementSession,
                                  uint64_t roundGeneration) const;
    // 方案A：请求全部保存器在队列排空后刷盘关闭当前会话文件（会话边界主动落盘）
    void     requestCloseSavers();
    // 更新生产输出边界使用的逻辑轮次计数（环形模式由 Ring 配置覆盖）。
    void setLogicalTriggersPerRound(std::uint64_t count);
    // Canonical physical-round idle timeout. Ring mode supplies
    // RingReconCudaConfig.timeoutResetSec; this setter is also valid before
    // the PAimage backend is created.
    void setPhysicalRoundTimeout(double seconds);
    // Physical-round startup policy. These are configuration-boundary
    // settings and are forwarded to the shared HostOutput normalizer.
    void setStartupFilterTriggerCount(std::uint64_t count);
    void setDisableCountBoundary(bool disable);
    paimage::PhysicalRoundNormalizer::Snapshot physicalRoundSnapshot() const;
    // 重新配置（采集时间改变时传入，无需重建线程）
    void reconfigure(const AcqConfig& config);

    //  UDP 控制命令（向采集卡发送，协议与 MC_410T_Qt 完全一致）
    //  增强版：自动等待卡片就绪 + 重试机制
    bool sendConfigCommand(int dataTime,
                           int aDelay,
                           int bDelay,
                           QString trigger = QStringLiteral("api"));
    bool sendStartMeasure();
    bool sendStopMeasure();
    // 测量门控：true=开始测量（处理并显示数据），false=停止测量（丢弃数据）
    // 仅保留给旧测试/兼容调用方；生产 Start/Stop 必须走 session transaction。
    void setMeasureEnabled(bool enable);

    //  状态查询
    bool isRunning()   const { return m_running; }
    bool isSaving()    const;
    int  activeCards() const { return m_config.nCards; }
    const AcqConfig& config() const { return m_config; }
    QString diagnosticRunId() const { return m_paimageRunId; }
    QString diagnosticRunDirectory() const { return m_paimageTracePath; }
    QString measurementSessionId() const { return m_measurementSessionId; }
    QVector<QString> diagnosticTargetIPs() const { return m_targetIPs; }

    //  卡就绪状态查询：卡上电 → FPGA 网络栈初始化 → 发送 18 字节就绪包后标记为就绪
    int  readyCardCount() const;
    bool isCardReady(int cardIdx) const;
    bool isAllCardsReady() const;

    // per-card 统计快照（主线程安全，~1Hz 调用）
    std::optional<CardStats::Snapshot> getCardStats(int cardIdx) const;
    std::vector<CardStats::Snapshot>   getAllCardStats() const;

    // 运行统计的稳定 machine-readable 字段，供诊断快照和单元测试共用。
    static QJsonObject runtimeStatsFields(const CardStats::Snapshot& stats);

    // DisplayBuffer 访问（MainWindow pull 模式）
    DisplayBuffer* displayBuffer(int cardIdx) const;

    // 统计更新（MainWindow::onStatsRefresh() 调用或由内部 QTimer 驱动）
    void updateAllStats();

signals:
    void statusMessage(const QString& msg);
    void errorOccurred(const QString& msg);
    void cardStatusChanged(int cardIdx, bool active);
    void cardReady(int cardIdx);            // 单张卡就绪（18字节版本号/就绪包到达）
    void allCardsReady();                   // 所有目标卡均已就绪
    void configAcked(int cardIdx);          // 单卡收到 60 字节配置反馈
    void configConfirmed();                 // 所有卡均收到 60 字节配置反馈
    void configAckFailed(int cardIdx);      // 单卡配置确认失败（重发超限）
    void measurementStarted(const QString& measurementSessionId);
    void measurementStartFailed(const QString& measurementSessionId,
                                const QString& reason);
    void measurementStopped(const QString& measurementSessionId,
                            bool commandSucceeded);
    void measurementStopFailed(const QString& measurementSessionId,
                               const QString& reason);
    void stopped();   // 所有子线程已退出，stop() 后台工作完成
    // started() 已移除，改用 start(config, onStarted回调) 方式通知 UI
    // 保存文件翻滚低频事件（容量 / 物理轮次），由 UI 桥接进诊断记录
    void fileSaverRollover(int cardId, const QString& reason,
                           quint64 oldRoundGeneration, quint64 newRoundGeneration,
                           int oldFileSequence, int newFileSequence,
                           int oldFileTriggerCount, bool manualMode);

private slots:
    void onStatsTimer();
    void pollPaimageLoopMonitor();

private:
    friend class NetworkDiagnosticTestAccess;
    bool startPaimage(const AcqConfig&,std::function<void()>,std::function<void()>);
    void stopPaimage();
    bool configurePaimage(int,int,int,const QString&);
    bool startPaimageMeasurement();
    bool stopPaimageMeasurement();
    void pollPaimage();
    bool createPaimageBackend(QString&);
    std::unique_ptr<paimage::TraceWriter> m_paimageTrace;
    std::unique_ptr<paimage::TimingWriter> m_paimageTiming;
    std::unique_ptr<paimage::LoopLog> m_paimageLoopLog;
    std::unique_ptr<paimage::Backend> m_paimage;
    QTimer* m_paimageTimer=nullptr;
    QTimer* m_paimageLoopMonitorTimer=nullptr;
    bool m_paimageStartPending=false;
    bool m_paimageConfigReported=false;
    quint64 m_paimageGeneration=0;
    QString m_paimageSaveDir,m_paimageSaveSuffix;
    QString m_paimageTracePath;
    int m_paimageSaveCount=1000;
    bool m_paimageSavingRequested=false;
    quint64 m_paimageSaveGeneration=0;
    bool m_paimageSaveAppliedLogged=false;
    std::vector<std::uint64_t> m_paimageLastBytes;
    quint64 m_paimageLastBurstEpoch=0;
    qint64 m_paimageLastStallWarnMs=0;
    QString m_paimageRunId;
    paimage::PhysicalRoundNormalizer::Observer m_physicalRoundBoundarySink;
    double m_physicalRoundTimeoutSec = 0.0;
    std::uint64_t m_startupFilterTriggerCount = 1;
    bool m_disableCountBoundary = false;
    void recordPaimageSnapshot();
    void writeSystemCaptureNotification(quint64 epoch, qint64 burstNs);

    // UDP 控制命令底层实现
    bool initControlSocket();
    void cleanupControlSocket();
    bool sendRawCommand(const QByteArray& cmd,
                        const QString& targetIP,
                        const QString& reason = QStringLiteral("first"));
    bool sendRawToAll(const QByteArray& cmd,
                      const QString& reason,
                      int* outSuccess,
                      int* outFail);
    QByteArray buildConfigPacket(int dataTime, int aDelay, int bDelay);

    void scheduleNetworkSnapshot(const QString& reason,
                                 const QString& phase = QString(),
                                 const QString& configId = QString());
    void recordIngressSnapshot(const QString& reason = QStringLiteral("runtime"));
    void recordCardSnapshots(const QString& stateOverride = QString());
    QString cardDiagnosticState(int cardIdx) const;
    void recordDiagnosticEvent(const QString& category,
                               const QString& message,
                               DiagnosticRecorder::Severity severity,
                               const QJsonObject& fields = QJsonObject()) const;

    // ══ 卡片就绪检测（被动监听 + 主动探测）═══════════════════════════
    bool initFeedbackListener();       // 在端口 8000 创建 UDP 监听 socket
    void cleanupFeedbackListener();
    void feedbackListenerThread();     // 后台线程：接收 18 字节就绪包
    void markCardReady(int globalCardIdx, bool viaReadyPacket);  // 主线程安全：标记单卡就绪（viaReadyPacket=是否真收到18字节包）
    void retryPendingCommand();        // 重试队列中的命令

    // ══ 主动探测（ARP 兜底，解决重启上位机后漏掉就绪包的问题）══════
    void startProbeTimer();            // 启动探测定时器（延迟 8 秒）
    void onProbeTimeout();             // 探测超时：对未就绪卡发起 ARP 探测

    // ══ 启动失败回滚 ══════════════════════════════════════════════
    void rollbackStart();              // 接收端口启动失败时同步清理本次 start() 的资源

    // ══ 命令重试机制 ════════════════════════════════════════════════
    enum class PendingCmdType { None, Config, StartMeasure, StopMeasure };
    // 底层发送函数（不做就绪检查，由重试机制/已就绪时直接调用）
    bool doSendConfigCommand(int dataTime, int aDelay, int bDelay);
    bool doSendStartMeasureCard(int cardIndex,
                                int* outSuccess = nullptr,
                                int* outFail = nullptr);
    bool doSendStopMeasure(int* outSuccess = nullptr, int* outFail = nullptr);
    QString newMeasurementSessionId();
    bool executeStartTransaction(const QString& measurementSessionId);
    bool executeStopTransaction(const QString& measurementSessionId);
    MeasurementSessionTransaction::TeardownResult
    resetProcessorsAfterSession(bool hardwareStopSucceeded);
    void recordSessionSettingsSnapshot(const QString& phase,
                                       const QJsonObject& fields) const;
    void clearPendingMeasurementRequests(const QString& reason);
    void setMeasurementBoundaryFault(const QString& reason);
    struct PendingCmd {
        PendingCmdType type = PendingCmdType::None;
        int  dataTime = 0;
        int  aDelay   = 0;
        int  bDelay   = 0;
        QString configId;
        QString trigger = QStringLiteral("api");
        QString measurementSessionId;
    };
    std::deque<PendingCmd> m_cmdQueue; // 待执行命令队列（按序：配置 → 测量，避免测量覆盖配置）
    QTimer*      m_retryTimer = nullptr; // 重试定时器（500ms）

    // ══ 配置确认状态机（60 字节反馈）═══════════════════════════════
    // 只有配置参数指令有 60 字节反馈；18 字节包不算配置成功，需重发。
    // 反馈按单卡独立上报，重发仅针对未确认的卡。
    enum class ConfigPhase { Idle, WaitingAck, Confirmed, Failed };
    struct PendingConfig {
        int dataTime = 0;
        int aDelay = 0;
        int bDelay = 0;
        QString configId;
        QString trigger = QStringLiteral("api");
    };
    void beginConfigWait(const PendingConfig& pc);  // 全部就绪后：逐卡下发并启动确认
    bool doSendConfigTo(int cardIdx,
                        const QString& reason = QStringLiteral("first")); // 仅向指定卡发送配置包并记录时间
    void onConfigAck(int cardIdx, quint64 receiveSequence); // 收到 60 字节反馈（主线程）
    void onReadyPacket(int cardIdx, quint64 receiveSequence); // 收到 18 字节包：标记就绪 + 等待确认时重发配置
    void onConfigTimerTick();                       // 200ms 轮询：超时重发 / 判失败
    bool isAllConfigAcked() const;
    bool isConfigConfirmed() const { return m_configPhase == ConfigPhase::Confirmed; }
    ConfigPhase          m_configPhase   = ConfigPhase::Idle;
    std::vector<bool>    m_configAck;     // 每卡是否收到 60 字节反馈
    std::vector<int>     m_configRetry;   // 每卡重发计数
    std::vector<uint64_t> m_configSentMs; // 每卡最近发送配置的时刻
    PendingConfig        m_pendingConfig; // 当前待确认的配置参数
    QTimer*              m_configTimer = nullptr; // 200ms 配置确认轮询

    struct CardDiagnosticState {
        QString discovery = QStringLiteral("unknown");
        QString localAddress = QStringLiteral("unknown");
        quint64 readyPacketCount = 0;
        quint64 arpReady = 0;
        quint64 sendCount = 0;
        quint64 ackPacketCount = 0;
        quint64 retryCount = 0;
        quint64 retryResetCount = 0;
    };
    std::vector<CardDiagnosticState> m_cardDiagnostics;
    QString m_diagnosticListenId;
    QString m_diagnosticSource = QStringLiteral("unknown");
    quint64 m_nextConfigId = 1;
    QString m_currentConfigId;
    QString m_currentConfigTrigger = QStringLiteral("api");
    quint64 m_nextMeasurementSessionId = 1;
    quint64 m_measurementSessionToken = 0;
    QString m_measurementSessionId;
    QString m_pendingMeasurementSessionId;
    bool m_measurementRunning = false;
    enum class MeasurementState { Disarmed, Preparing, Armed, Running, Stopping, Fault };
    MeasurementState m_measurementState = MeasurementState::Disarmed;
    QString m_measurementFaultReason;
    std::atomic<quint64> m_feedbackSequence{0};
    std::atomic<quint64> m_feedbackTimeoutCount{0};

    AcqConfig m_config;
    bool      m_running = false;
    std::thread m_stopThread;          // 后台等待线程（stop() 在此线程里做 wait/terminate）

    std::vector<std::unique_ptr<DataProcessor>>    m_processors;
    DataProcessor::RingFeedSink                   m_ringFeedSink;   // 环形实时馈送回调
    std::vector<std::unique_ptr<DisplayBuffer>>    m_displayBuffers;
    std::vector<std::unique_ptr<FileSaver>>        m_savers;

    // ══ 自动保存会话代（物理边界协调器）══════════════════════════
    paimage::AutoSaveRoundCoordinator m_autoSaveCoordinator;

    std::vector<std::unique_ptr<MultiPortReceiver>> m_receivers;

    std::unique_ptr<FramePublisher> m_publisher;

    // UDP 控制 socket
    SocketType            m_controlSocket = INVALID_SOCKET;
    QVector<QString>      m_targetIPs;    // 每张卡的 IP，192.168.0.2 起

    // ══ 反馈监听（端口 8000）══════════════════════════════════════
    SocketType            m_feedbackSocket = INVALID_SOCKET;
    std::thread           m_feedbackThread;  // 后台就绪包监听线程
    std::atomic<bool>     m_feedbackRunning{false};
    std::vector<bool>     m_cardsReady;       // per-card 就绪标志

    // ══ 主动探测（ARP 兜底）═══════════════════════════════════════
    QTimer*               m_probeTimer = nullptr; // 8 秒后触发 ARP 探测
    int                   m_probeRetryCount = 0;  // 探测重试计数

    // 统计采样（差分法计算速率）
    QTimer*               m_statsTimer = nullptr;
    std::vector<uint64_t> m_lastPktsReceived;
    std::vector<uint64_t> m_lastPktsDropped;
    std::vector<uint64_t> m_lastTrigsComplete;
    uint64_t              m_lastStatsMs = 0;
    uint64_t              m_lastRuntimeSnapshotMs = 0;
    uint64_t              m_lastIngressSnapshotMs = 0;
    NetworkDiagnostics::IngressSampler m_ingressSampler;
};
