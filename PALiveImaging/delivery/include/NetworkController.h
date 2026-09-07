#pragma once
#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <QStringList>
#include <QVector>
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
#include "FramePublisher.h"
#include "DisplayBuffer.h"

#ifdef _WIN32
    #include <winsock2.h>
    typedef SOCKET SocketType;
#else
    #include <sys/socket.h>
    typedef int SocketType;
    #define INVALID_SOCKET -1
#endif

#include "MultiPortReceiver.h"

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

    //  主接口（MainWindow 调用）
    // onStarted：初始化完成后的回调（同步调用，可直接操作 UI）
    // onFailed：初始化失败时调用，可为 nullptr
    void start(const AcqConfig& config,
               std::function<void()> onStarted = nullptr,
               std::function<void()> onFailed  = nullptr);

    // stop()：立即发出停止信号并返回（非阻塞）
    // 线程的实际等待/terminate 在后台 std::thread 完成，完成后发出 stopped() 信号
    void stop();

    //  网段扫描：探测 baseIP 起 count 个 IP 的可达性，返回在线 IP 列表
    //  专用网段假设：在线 IP 即采集卡（上位机/交换机避开扫描范围），
    //  用于自动识别采集卡数量与目标 IP。扫描范围由注册表
    //  NetworkParams/ScanBaseIP、ScanIPCount 控制（默认 .2 起 32 个）。
    static QVector<QString> scanReachableIPs(const QString& baseIP, int count);

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
    // 实时更新显示降采样点数（不重建线程，直接修改各 DataProcessor 的配置）
    void setDisplayPoints(int displayPoints);
    // 重新配置（采集时间改变时传入，无需重建线程）
    void reconfigure(const AcqConfig& config);

    //  UDP 控制命令（向采集卡发送，协议与 MC_410T_Qt 完全一致）
    //  增强版：自动等待卡片就绪 + 重试机制
    bool sendConfigCommand(int dataTime, int aDelay, int bDelay);
    bool sendStartMeasure();
    bool sendStopMeasure();

    //  状态查询
    bool isRunning()   const { return m_running; }
    bool isSaving()    const;
    int  activeCards() const { return m_config.nCards; }
    const AcqConfig& config() const { return m_config; }

    //  卡就绪状态查询：卡上电 → FPGA 网络栈初始化 → 发送 18 字节就绪包后标记为就绪
    int  readyCardCount() const;
    bool isCardReady(int cardIdx) const;
    bool isAllCardsReady() const;

    // per-card 统计快照（主线程安全，~1Hz 调用）
    std::optional<CardStats::Snapshot> getCardStats(int cardIdx) const;
    std::vector<CardStats::Snapshot>   getAllCardStats() const;

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
    void stopped();   // 所有子线程已退出，stop() 后台工作完成
    // started() 已移除，改用 start(config, onStarted回调) 方式通知 UI

private slots:
    void onStatsTimer();

private:
    // UDP 控制命令底层实现
    bool initControlSocket();
    void cleanupControlSocket();
    bool sendRawCommand(const QByteArray& cmd, const QString& targetIP);
    QByteArray buildConfigPacket(int dataTime, int aDelay, int bDelay);

    // ══ 卡片就绪检测（被动监听 + 主动探测）═══════════════════════════
    bool initFeedbackListener();       // 在端口 8000 创建 UDP 监听 socket
    void cleanupFeedbackListener();
    void feedbackListenerThread();     // 后台线程：接收 18 字节就绪包
    void markCardReady(int globalCardIdx, bool viaReadyPacket);  // 主线程安全：标记单卡就绪（viaReadyPacket=是否真收到18字节包）
    void retryPendingCommand();        // 重试队列中的命令

    // ══ 主动探测（ARP 兜底，解决重启上位机后漏掉就绪包的问题）══════
    void startProbeTimer();            // 启动探测定时器（延迟 8 秒）
    void onProbeTimeout();             // 探测超时：对未就绪卡发起 ARP 探测

    // ══ 命令重试机制 ════════════════════════════════════════════════
    enum class PendingCmdType { None, Config, StartMeasure, StopMeasure };
    // 底层发送函数（不做就绪检查，由重试机制/已就绪时直接调用）
    bool doSendConfigCommand(int dataTime, int aDelay, int bDelay);
    bool doSendStartMeasure();
    bool doSendStopMeasure();
    struct PendingCmd {
        PendingCmdType type = PendingCmdType::None;
        int  dataTime = 0;
        int  aDelay   = 0;
        int  bDelay   = 0;
    };
    std::deque<PendingCmd> m_cmdQueue; // 待执行命令队列（按序：配置 → 测量，避免测量覆盖配置）
    QTimer*      m_retryTimer = nullptr; // 重试定时器（500ms）

    // ══ 配置确认状态机（60 字节反馈）═══════════════════════════════
    // 只有配置参数指令有 60 字节反馈；18 字节包不算配置成功，需重发。
    // 反馈按单卡独立上报，重发仅针对未确认的卡。
    enum class ConfigPhase { Idle, WaitingAck, Confirmed, Failed };
    struct PendingConfig { int dataTime = 0; int aDelay = 0; int bDelay = 0; };
    void beginConfigWait(const PendingConfig& pc);  // 全部就绪后：逐卡下发并启动确认
    bool doSendConfigTo(int cardIdx);               // 仅向指定卡发送配置包并记录时间
    void onConfigAck(int cardIdx);                  // 收到 60 字节反馈（主线程）
    void onReadyPacket(int cardIdx);                // 收到 18 字节包：标记就绪 + 等待确认时重发配置
    void onConfigTimerTick();                       // 200ms 轮询：超时重发 / 判失败
    bool isAllConfigAcked() const;
    bool isConfigConfirmed() const { return m_configPhase == ConfigPhase::Confirmed; }
    ConfigPhase          m_configPhase   = ConfigPhase::Idle;
    std::vector<bool>    m_configAck;     // 每卡是否收到 60 字节反馈
    std::vector<int>     m_configRetry;   // 每卡重发计数
    std::vector<uint64_t> m_configSentMs; // 每卡最近发送配置的时刻
    PendingConfig        m_pendingConfig; // 当前待确认的配置参数
    QTimer*              m_configTimer = nullptr; // 200ms 配置确认轮询

    AcqConfig m_config;
    bool      m_running = false;
    std::thread m_stopThread;          // 后台等待线程（stop() 在此线程里做 wait/terminate）

    std::vector<std::unique_ptr<DataProcessor>>    m_processors;
    DataProcessor::RingFeedSink                   m_ringFeedSink;   // 环形实时馈送回调
    std::vector<std::unique_ptr<DisplayBuffer>>    m_displayBuffers;
    std::vector<std::unique_ptr<FileSaver>>        m_savers;

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
};
