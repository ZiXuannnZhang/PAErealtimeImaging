#pragma once

#include <QObject>
#include <QSharedMemory>
#include <QVector>
#include <QTimer>
#include <QJsonObject>
#include "ImagingParams.h"

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
    void processPulse();
    void sendFrameToHost();
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

    bool m_initialized;
    bool m_running;
};
