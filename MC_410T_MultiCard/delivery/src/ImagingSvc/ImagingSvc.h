#pragma once

#include <QObject>
#include <QSharedMemory>
#include <QVector>
#include <QTimer>
#include <QJsonObject>
#include <vector>
#include "ImagingParams.h"
#include "RingShmObservability.h"
#include "RoundIdentity.h"
#include "ring_recon_cuda.h"

namespace zmq { class context_t; class socket_t; }
namespace pa_recon_qt { class PAReconstructor; struct Config; }

// =====================================================================
// ImagingSvc — GPU 子进程成像服务
//
// 职责：
//   1. 通过 ZMQ PAIR 接收主进程信令（configure / start / stop / pulse_ready）
//   2. 从 QSharedMemory 读取脉冲数据
//   3. 调用 pa_recon_qt::PAReconstructor 进行 GPU 加速图像重建
//   4. 将重建帧写回共享内存并通知主进程
//
// 对应《成像功能扩展设计方案》v2.0 §3.4
// =====================================================================
class ImagingSvc : public QObject
{
    Q_OBJECT
public:
    explicit ImagingSvc(QObject *parent = nullptr);
    ~ImagingSvc();

    bool initialize();

private slots:
    void pollControlMessages();

private:
    void processMessage(const QJsonObject &msg);
    void processConfigure(const QJsonObject &params);
    void processRingConfigure(const QJsonObject &ring);
    void processPulse();
    void processRingPulse(uint32_t notifySeq, uint64_t submitIndex,
                          uint64_t submitWallUs, bool notifySeqValid,
                          const paimage::RoundIdentity &round);
    void resetRingRecon();        // 圈末/超时共用：清空 CUDA 累积与跨圈边界状态
    void sendRingObservation(const char *kind,
                             const ring_shm_obs::Snapshot &snapshot,
                             const QJsonObject &extra = QJsonObject());
    void sendFrameToHost();
    void sendRingSnapshotToHost(uint64_t submitIndex,
                                const paimage::RoundIdentity &round);
    void sendStatus(float fps);
    void sendError(const QString &errMsg, int code);

    pa_recon_qt::PAReconstructor *m_reconstructor;
    QSharedMemory *m_sharedMemory;
    zmq::context_t *m_zmqCtx;
    zmq::socket_t  *m_zmqSocket;
    QTimer         *m_pollTimer;

    QVector<float> m_pulseBuffer;
    int m_pulseCount;

    GeneralParams m_generalParams;
    ScanParams    m_scanParams;
    ReconParams   m_reconParams;

    // 环形扫描并行分支
    RingReconCudaConfig m_ringConfig;
    int                 m_ringSysDelayCh[8][2] = {
        {358, 371}, {358, 371}, {358, 371}, {358, 371},
        {358, 371}, {358, 371}, {358, 371}, {358, 371}};   // 每通道双波长延时截断（实时预处理使用）
    bool                m_ringMode = false;
    void               *m_ringCuda[2] = {nullptr, nullptr};
    QSharedMemory      *m_ringSharedMemory = nullptr;
    int                 m_ringBlockSize = 0;
    int                 m_ringFrameSize = 0;
    int                 m_ringBlockIndex = 0;
    int                 m_ringAlineCount = 0;
    int                 m_ringChannelCount = 0;
    int                 m_ringBlocksPerFrame = 1;
    int                 m_ringDisplayNx = 0;        // 方案A：= nx（显示=全分辨率）
    int                 m_ringDisplayStep = 1;
    int                 m_ringDisplayFrameSize = 0;
    std::vector<float>  m_ringDisplay0;             // nx*nx 归一化显示帧
    std::vector<float>  m_ringDisplay1;
    int                 m_ringChannels[8] = {0};
    std::vector<float>  m_ringPrevWL2[8];   // per-channel prev wl2 A-line (preprocessed)
    float               m_ringPrevAngle[8] = {0.0f};
    float               m_ringPrevRadius[8] = {0.0f};  // per-channel prev wl2 半径（多半径配准跨块对齐）
    ring_shm_obs::Tracker m_ringObs;

    bool m_initialized;
    bool m_running;
};
