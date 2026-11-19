#pragma once

#include <QObject>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#ifdef RING_INPUT_TEST_SEAM
#include <functional>
#endif
#include <QProcess>
#include <QSharedMemory>
#include <QVector>
#include <QImage>
#include <QMutex>
#include <QJsonObject>
#include <QTimer>
#include <vector>
#include <deque>
#include <chrono>
#include "RingNativeMapping.h"
#include "ImagingParams.h"
#include "RingTimevarLowpassConfig.h"
#include "RingFbpSettings.h"
#include "RingShmObservability.h"
#include "RingSnapshotCopy.h"
#include "RingTerminalSnapshot.h"
#include <memory>
#include "RoundIdentity.h"
#include "ring_recon_cuda.h"

namespace zmq { class context_t; class socket_t; }

// =====================================================================
// ImagingController — 主进程端成像控制器
//
// 职责：
//   1. 管理 ImagingSvc 子进程生命周期（启动/停止/崩溃重启）
//   2. 通过 ZMQ PAIR 下发配置和信令
//   3. 通过 QSharedMemory 传输脉冲数据和接收重建图像
//   4. 将 float 帧数据转换为 QImage 供 UI 显示
//
// 对应《成像功能扩展设计方案》v2.0 §3.3
// =====================================================================
class ImagingController : public QObject
{
    Q_OBJECT
public:
    explicit ImagingController(QObject *parent = nullptr, bool singlePreviewInFlight = false);
    ~ImagingController();

    // 生命周期
    bool startSvc();
    void stopSvc();
    bool isRunning() const;
    bool hasActiveService() const { return m_svcResourcesActive; }

    // 配置（在 startSvc 之前调用）
    void configure(const GeneralParams &general,
                   const ScanParams &scan,
                   const ReconParams &recon);

    // 环形扫描模式配置（与线性 pa_recon 分支并行；调用后 startSvc 走环形链路）
    // sysDelayCh 为每通道双波长延时截断（nullptr=回退为 cfg.sysDelay 广播到所有通道）
    bool configureRing(const RingReconCudaConfig &ringCfg,
                       const RingTimevarLowpassConfig &timevarCfg,
                       const int (*sysDelayCh)[2] = nullptr,
                       const RingFbpSettings &fbp = RingFbpSettings{});
    bool isRingMode() const { return m_ringMode; }
    void applyMotorRingDirection(int sectorCcw, bool updateOrigin = true);
    const RingReconCudaConfig &ringConfig() const { return m_ringConfig; }
    const RingTimevarLowpassConfig &ringTimevarConfig() const { return m_ringTimevarConfig; }

    // 环形扫描：提交一个原始 A-line 块（float32，sampDepth x alinesPerBlock，列主序）
    // sourceRoundComplete 表示该块所属物理轮的最后逻辑触发已被上游观察到
    // （source round end），不等价于服务端重建完整。
    bool submitRingBlock(const QVector<float> &rawBlock,
                         const QVector<float> &anglesDeg,
                         const QVector<quint8> &channels,
                         const paimage::RoundIdentity &round,
                         int blockSeq,
                         std::uint64_t *outSubmitIndex = nullptr,
                         bool sourceRoundComplete = false, int sectorCcw = -1);
    // 超时判定新一圈：通知子进程清空重建累积（RingBlockAssembler 超时回调调用）
    bool sendRingReset();
#ifdef RING_INPUT_TEST_SEAM
    void testFailNextRingNotifications(int count) { m_testFailRingNotifications.store(count); }
#endif

    // 数据输入（从 MainWindow 调用，线程安全）
    // freqA/freqB 分别对应一张卡的两个物理通道的频率数据
    // F64 模式：每个通道含 8 根光纤 × depth 个采样点
    // F8 模式：每个通道含 1 根光纤 × depth 个采样点
    void feedPulseData(int cardId, uint16_t triggerSeq,
                       const QVector<float> &freqA,
                       const QVector<float> &freqB = QVector<float>());
    void flushPulseData();  // 将累积的多卡脉冲数据提交给子进程

    // 获取最新重建图像
    QImage getLatestImage() const;
    QVector<float> getLatestFrameData() const;
    int frameWidth() const;
    int frameHeight() const;

    int ringBlocksPerFrame() const;   // 配置块数，仅用于进度显示，完成由 round_complete 决定
    int ringDisplayNx() const { return m_ringLayout.nx ? m_ringLayout.nx : m_pendingRingLayout.nx; }
    std::uint64_t ringLastSubmitIndex() const;

    // A published buffer is immutable until releaseSnapshotBuffer. The
    // preview worker keeps one latest request while publication is blocked.
    const float *snapshotBuffer(int index) const;
    void releaseSnapshotBuffer(int index);
    std::shared_ptr<std::vector<float>> terminalSnapshotBuffer(int index) const;

    quint64 serviceLifecycleToken() const noexcept { return m_svcLifecycleToken; }

signals:
    void imageReady(const QImage &image, int seq);
    void ringTerminalUnavailable(quint64 measurement, quint64 round, quint64 submitIndex, QString reason);
    void ringSnapshotReady(int seq,
                           quint64 submitIndex,
                           bool hasSubmitIndex,
                           quint64 measurementSession,
                           quint64 roundGeneration,
                           bool roundComplete, int bufferIndex);  // 每块显示快照（UI 线程槽）
    void svcStatus(const QString &status, float fps);
    void svcError(const QString &error);
    void ringInputFailed(); // queued onto the owner thread before delivery
    void svcReady();                   // 子进程就绪（配置已下发）
    void svcStopped();                 // 异步停止完成通知
    void ringConfigChangedWhileRunning();  // 运行中环形参数变更（需重启子进程生效）
    void ringDirectionChanged(int sectorCcw);

private slots:
    void onSvcStarted();
    void onSvcFinished(int exitCode, QProcess::ExitStatus status);
    void onSvcErrorOccurred(QProcess::ProcessError error);
    void pollControlMessages();

private:
    bool sendCommand(const QJsonObject &cmd);
    void sendConfigureAndStart();
    QImage frameDataToImage(const QVector<float> &data, int nx, int ny) const;
    void setupSharedMemory(int pulseSize, int frameSize);
    struct RingLayout {
        int nx = 0, blockSize = 0, alines = 0, frameSize = 0;
        ring_snapshot::Layout bytes;
        size_t capacity = 0;
    };
    bool setupRingSharedMemory(RingLayout layout, const wchar_t *name = ring_input::kNativeMappingName);
    void closeRingAdmission();
    void pumpRingInput();
    void clearRingInput();
    void processMessage(const QJsonObject &msg);
    void processRingMessage(const QJsonObject &msg);
    void startRingFrameWorker(int seq,
                              std::uint64_t submitIndex,
                              bool hasSubmitIndex,
                              const paimage::RoundIdentity &round, bool roundComplete);   // 请求环形帧转换（最新一帧覆盖）
    void ensureRingWorkerStarted();
    void stopRingWorker();
    void ringWorkerLoop(quint64 lifecycle, const RingLayout &layout);
    void processRingTerminal(quint64 lifecycle, const RingLayout &layout);
    void processRingFrame(int seq,
                          std::uint64_t submitIndex,
                          bool hasSubmitIndex,
                          const paimage::RoundIdentity &round, bool roundComplete,
                          quint64 lifecycle, const RingLayout &layout);       // 常驻工作线程内的单帧读取/转换/投递
    void finishStopSvc();              // 异步停止的收尾清理

    QProcess      *m_svcProcess;
#ifdef MAINWINDOW_START_TEST_SEAM
    friend class MainWindowStartIntegrationTest;
    int m_testSvcStarts = 0;
    int m_testSvcStops = 0;
#endif
    QSharedMemory *m_sharedMemory;

    zmq::context_t *m_zmqCtx;
    zmq::socket_t  *m_zmqSocket;
    std::mutex      m_zmqMutex;      // 串行化 ZMQ send/recv（工作线程与 UI 线程并发访问）
    QTimer         *m_pollTimer;
    bool            m_suppressSvcCrashError = false;  // 主动停止/重启时抑制误报的崩溃错误

    GeneralParams  m_generalParams;
    ScanParams     m_scanParams;
    ReconParams    m_reconParams;

    // 环形扫描并行分支
    RingReconCudaConfig m_ringConfig; // pending settings, owner thread only
    RingLayout m_pendingRingLayout;
    RingLayout m_ringLayout; // installed under queue lock; immutable until worker joins
    std::atomic<bool> m_ringAdmission{false};
    RingTimevarLowpassConfig   m_ringTimevarConfig;
    RingFbpSettings m_fbpSettings;
    std::atomic<bool> m_fbpPreparing{false};
    quint64 m_fbpPrepareToken=0;
    quint64 m_svcLifecycleToken = 0;
    bool m_stopping = false;
    bool m_svcResourcesActive = false;
    int                 m_ringSysDelayCh[8][2] = {
        {358, 371}, {358, 371}, {358, 371}, {358, 371},
        {358, 371}, {358, 371}, {358, 371}, {358, 371}};   // 每通道双波长延时截断（独立于 CUDA 结构体，避免 ABI 变更）
    bool                m_ringMode = false;
    ring_input::NativeMapping m_ringMapping;
    ring_input::Mutex m_ringInputMutex;
#ifdef RING_INPUT_TEST_SEAM
    std::atomic<int> m_testFailRingNotifications{0};
    friend struct RingLayoutRegression;
    std::function<void()> m_testBeforeRingCopy;
#endif
    ring_input::Mutex m_ringOutputMutex{ring_input::kOutputMutexName};
    ring_input::Mutex m_ringTerminalMutex{ring_terminal::kMutexName};
    struct RingBlock {
        QVector<float> raw, angles;
        QVector<quint8> channels;
        paimage::RoundIdentity round;
        std::uint64_t index = 0, wallUs = 0;
        int seq = 0, direction = -1;
        bool complete = false;
        uint32_t roundBlockIndex = 0;
    };
    struct RingRejection {
        paimage::RoundIdentity round;
        std::uint64_t epoch, afterIndex;
        uint32_t first, last;
        int direction;
        bool terminal;
    };
    std::deque<RingRejection> m_ringRejections;
    bool m_ringRejectionOverflow = false;
    std::chrono::steady_clock::time_point m_ringRejectionDeadline = std::chrono::steady_clock::time_point::max();
    mutable std::mutex m_ringQueueMutex;
    std::deque<RingBlock> m_ringQueue;
    paimage::RoundIdentity m_ringBaselineRound; // protected by m_ringQueueMutex
    int m_ringBaselineSeq = 0;
    std::vector<std::deque<RingBlock>> m_ringRetiredQueues;
    std::uint64_t m_ringAcceptedCount = 0, m_ringRejectedCount = 0;
    std::uint64_t m_ringReportedAccepted = 0, m_ringReportedRejected = 0;
    size_t m_ringRetiredBytes = 0, m_ringRetiredInFlightBytes = 0;
    std::uint64_t m_ringSubmitIndex = 0;
    std::uint64_t m_ringRequestedEpoch = 0;
    bool m_ringInputFailed = false;
    size_t m_ringPendingBytes = 0, m_ringPendingCount = 0;
    // The following transport state belongs exclusively to the timer thread.
    std::uint64_t m_ringActiveEpoch = 0, m_ringResetTarget = 0;
    RingBlock m_ringInFlight;
    bool m_ringHasInFlight = false, m_ringPublished = false, m_ringResetPending = false;
    std::chrono::steady_clock::time_point m_ringDeadline, m_ringNextNotify;
    ring_shm_obs::Tracker m_ringObs;

    QVector<float> m_latestFrame;
    mutable QMutex m_frameMutex;
    int            m_frameWidth;
    int            m_frameHeight;
    std::atomic<bool> m_running{false};

    // 方案A：固定显示双缓冲（各 nx²*2 float，运行期零分配）
    const bool m_singlePreviewInFlight;
    std::vector<float> m_dispBuf[2];
    // Indices 2 and 3 on ringSnapshotReady. A retained shared_ptr keeps the
    // buffer unavailable until asynchronous PNG writing has finished.
    std::shared_ptr<std::vector<float>> m_terminalBuffers[ring_terminal::kSlots];
    std::atomic<bool> m_terminalConsumed[ring_terminal::kSlots]{{true}, {true}};
    std::atomic<int>   m_dispActive{-1};        // 最后发布缓冲（-1=无）
    std::atomic<int>   m_dispSeq[2] = {{-1}, {-1}};  // 各缓冲的帧序号
    std::atomic<bool>  m_dispConsumed[2] = {{true}, {true}};  // UI 读完置 true，worker 写入前取反
    // 环形帧异步读取：常驻单工作线程（首次有帧时创建，按帧序号通知处理），
    // 同一时刻至多处理一帧；停止时先通知退出再汇合，再释放共享内存，杜绝悬空访问。
    // 相比“每帧创建/汇合一个 std::thread”，消除每帧闭包分配/释放与线程 churn，
    // 排除分配器复用被污染闭包块这一崩溃来源。
    std::thread          m_ringWorker;
    std::mutex           m_ringWorkerMutex;   // 保护 m_ringWorker 的创建/汇合
    std::mutex           m_ringReqMutex;      // 帧请求（最新一帧覆盖）
    std::condition_variable m_ringReqCv;
    int                  m_ringReqSeq = -1;
    std::uint64_t        m_ringReqSubmitIndex = 0;
    bool                 m_ringReqHasSubmitIndex = false;
    paimage::RoundIdentity m_ringReqRound;
    bool m_ringReqRoundComplete = false;
    bool                 m_ringReqPending = false;
    bool                 m_ringWorkerStop = false;

    // 多卡脉冲聚合缓冲
    QVector<float> m_pulseAccumBuf;
    int            m_pulseAccumCount;
    int            m_pulseAccumSeq;
};
