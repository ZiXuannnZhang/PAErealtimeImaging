#pragma once

#include <QObject>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <QProcess>
#include <QSharedMemory>
#include <QVector>
#include <QImage>
#include <QMutex>
#include <QJsonObject>
#include <QTimer>
#include <vector>
#include "ImagingParams.h"
#include "RingShmObservability.h"
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
    explicit ImagingController(QObject *parent = nullptr);
    ~ImagingController();

    // 生命周期
    bool startSvc();
    void stopSvc();
    bool isRunning() const;

    // 配置（在 startSvc 之前调用）
    void configure(const GeneralParams &general,
                   const ScanParams &scan,
                   const ReconParams &recon);

    // 环形扫描模式配置（与线性 pa_recon 分支并行；调用后 startSvc 走环形链路）
    // sysDelayCh 为每通道双波长延时截断（nullptr=回退为 cfg.sysDelay 广播到所有通道）
    void configureRing(const RingReconCudaConfig &ringCfg,
                       const int (*sysDelayCh)[2] = nullptr);
    bool isRingMode() const { return m_ringMode; }
    const RingReconCudaConfig &ringConfig() const { return m_ringConfig; }

    // 环形扫描：提交一个原始 A-line 块（float32，sampDepth x alinesPerBlock，列主序）
    // sourceRoundComplete 表示该块所属物理轮的最后逻辑触发已被上游观察到
    // （source round end），不等价于服务端重建完整。
    bool submitRingBlock(const QVector<float> &rawBlock,
                         const QVector<float> &anglesDeg,
                         const QVector<quint8> &channels,
                         const paimage::RoundIdentity &round,
                         int blockSeq,
                         std::uint64_t *outSubmitIndex = nullptr,
                         bool sourceRoundComplete = false);
    // 超时判定新一圈：通知子进程清空重建累积（RingBlockAssembler 超时回调调用）
    bool sendRingReset();

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
    int ringDisplayNx() const { return m_ringDisplayNx; }  // 方案A：= nx（显示=全分辨率）
    std::uint64_t ringLastSubmitIndex() const;

    // 固定双缓冲访问（仅 UI 线程调用）：
    // snapshotBuffer 返回已发布缓冲的只读指针，使用完必须 releaseSnapshotBuffer，
    // 否则工作线程将丢弃后续快照（latest-wins）。
    const float *snapshotBuffer(int index) const;
    void releaseSnapshotBuffer(int index);

signals:
    void imageReady(const QImage &image, int seq);
    void ringSnapshotReady(int seq,
                           quint64 submitIndex,
                           bool hasSubmitIndex,
                           quint64 measurementSession,
                           quint64 roundGeneration,
                           bool roundComplete, int bufferIndex);  // 每块显示快照（UI 线程槽）
    void svcStatus(const QString &status, float fps);
    void svcError(const QString &error);
    void svcReady();                   // 子进程就绪（配置已下发）
    void svcStopped();                 // 异步停止完成通知
    void ringConfigChangedWhileRunning();  // 运行中环形参数变更（需重启子进程生效）

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
    bool setupRingSharedMemory(int blockSize, int frameSize, int alines, int nx);
    bool ringShmMatches(int blockSize, int frameSize, int alines, int nx,
                        size_t expectedTotal) const;  // v2 强校验
    void processMessage(const QJsonObject &msg);
    void processRingMessage(const QJsonObject &msg);
    void startRingFrameWorker(int seq,
                              std::uint64_t submitIndex,
                              bool hasSubmitIndex,
                              const paimage::RoundIdentity &round, bool roundComplete);   // 请求环形帧转换（最新一帧覆盖）
    void ensureRingWorkerStarted();
    void stopRingWorker();
    void ringWorkerLoop();
    void processRingFrame(int seq,
                          std::uint64_t submitIndex,
                          bool hasSubmitIndex,
                          const paimage::RoundIdentity &round, bool roundComplete);       // 常驻工作线程内的单帧读取/转换/投递
    void finishStopSvc();              // 异步停止的收尾清理

    QProcess      *m_svcProcess;
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
    RingReconCudaConfig m_ringConfig;
    int                 m_ringSysDelayCh[8][2] = {
        {358, 371}, {358, 371}, {358, 371}, {358, 371},
        {358, 371}, {358, 371}, {358, 371}, {358, 371}};   // 每通道双波长延时截断（独立于 CUDA 结构体，避免 ABI 变更）
    bool                m_ringMode = false;
    QSharedMemory      *m_ringSharedMemory = nullptr;
    int                 m_ringBlockSize = 0;
    int                 m_ringAlineCount = 0;
    int                 m_ringFrameSize = 0;
    int                 m_ringDisplayNx = 0;        // 显示网格 dn
    int                 m_ringDisplayStep = 0;      // 显示降采样步长
    int                 m_ringDisplayFrameSize = 0; // dn*dn
    ring_shm_obs::Tracker m_ringObs;

    QVector<float> m_latestFrame;
    mutable QMutex m_frameMutex;
    int            m_frameWidth;
    int            m_frameHeight;
    std::atomic<bool> m_running{false};

    // 方案A：固定显示双缓冲（各 nx²*2 float，运行期零分配）
    std::vector<float> m_dispBuf[2];
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
