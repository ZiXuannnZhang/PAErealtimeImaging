#pragma once

#include <QObject>
#include <mutex>
#include <QProcess>
#include <QSharedMemory>
#include <QVector>
#include <QImage>
#include <QMutex>
#include <QJsonObject>
#include <QTimer>
#include "ImagingParams.h"
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
    void configureRing(const RingReconCudaConfig &ringCfg);
    bool isRingMode() const { return m_ringMode; }
    const RingReconCudaConfig &ringConfig() const { return m_ringConfig; }

    // 环形扫描：提交一个原始 A-line 块（float32，sampDepth x alinesPerBlock，列主序）
    bool submitRingBlock(const QVector<float> &rawBlock, const QVector<float> &anglesDeg, const QVector<quint8> &channels, int blockSeq);

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

    // 环形双波长最新帧（供独立成像弹窗读取）
    QVector<float> latestRingFrame(int index) const;  // 0=532nm, 1=1064nm
    int ringFrameNx() const;

signals:
    void imageReady(const QImage &image, int seq);
    void ringImageReady(const QImage &wl1, const QImage &wl2, int seq);
    void svcStatus(const QString &status, float fps);
    void svcError(const QString &error);
    void svcReady();                   // 子进程就绪（配置已下发）
    void svcStopped();                 // 异步停止完成通知

private slots:
    void onSvcStarted();
    void onSvcFinished(int exitCode, QProcess::ExitStatus status);
    void onSvcErrorOccurred(QProcess::ProcessError error);
    void pollControlMessages();

private:
    void sendCommand(const QJsonObject &cmd);
    void sendConfigureAndStart();
    QImage frameDataToImage(const QVector<float> &data, int nx, int ny) const;
    void setupSharedMemory(int pulseSize, int frameSize);
    void setupRingSharedMemory(int blockSize, int frameSize, int alines);
    void processMessage(const QJsonObject &msg);
    void processRingMessage(const QJsonObject &msg);
    void finishStopSvc();              // 异步停止的收尾清理

    QProcess      *m_svcProcess;
    QSharedMemory *m_sharedMemory;

    zmq::context_t *m_zmqCtx;
    zmq::socket_t  *m_zmqSocket;
    std::mutex      m_zmqMutex;      // 串行化 ZMQ send/recv（工作线程与 UI 线程并发访问）
    QTimer         *m_pollTimer;

    GeneralParams  m_generalParams;
    ScanParams     m_scanParams;
    ReconParams    m_reconParams;

    // 环形扫描并行分支
    RingReconCudaConfig m_ringConfig;
    bool                m_ringMode = false;
    QSharedMemory      *m_ringSharedMemory = nullptr;
    int                 m_ringBlockSize = 0;
    int                 m_ringAlineCount = 0;
    int                 m_ringFrameSize = 0;

    QVector<float> m_latestFrame;
    mutable QMutex m_frameMutex;
    int            m_frameWidth;
    int            m_frameHeight;
    bool           m_running;

    // 环形双波长最新帧缓存
    QVector<float> m_latestRingFrames[2];
    int            m_ringFrameNx = 0;
    mutable QMutex m_ringFrameMutex;

    // 多卡脉冲聚合缓冲
    QVector<float> m_pulseAccumBuf;
    int            m_pulseAccumCount;
    int            m_pulseAccumSeq;
};
