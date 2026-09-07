#include "ImagingController.h"
#include "ImagingSharedMemory.h"
#include <zmq.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QTimer>
#include <QFile>
#include <QThread>
#include <cmath>
#include <cstring>

// ZMQ IPC 端点（与 ImagingSvc 子进程约定一致）
static const char *ZMQ_IPC_ENDPOINT = "tcp://127.0.0.1:5555";

// =====================================================================
// 构造 / 析构
// =====================================================================
ImagingController::ImagingController(QObject *parent)
    : QObject(parent)
    , m_svcProcess(nullptr)
    , m_sharedMemory(nullptr)
    , m_zmqCtx(nullptr)
    , m_zmqSocket(nullptr)
    , m_pollTimer(nullptr)
    , m_frameWidth(0)
    , m_frameHeight(0)
    , m_running(false)
    , m_ringSharedMemory(nullptr)
    , m_pulseAccumCount(0)
    , m_pulseAccumSeq(0)
{
}

ImagingController::~ImagingController()
{
    stopSvc();
}

// =====================================================================
// 生命周期管理
// =====================================================================
bool ImagingController::startSvc()
{
    if (m_running) return true;

    // 总脉冲数据量 = physicalChannels × depth（8 物理通道）
    int pulseSize = m_generalParams.physicalChannels * m_generalParams.depth;
    int frameSize = m_scanParams.nx * m_scanParams.ny;

    if (m_ringMode) {
        // 环形扫描并行分支：块输入 + 双波长双帧输出
        const int nx = static_cast<int>(std::ceil(m_ringConfig.fov / m_ringConfig.gridSize));
        const int alines = m_ringConfig.enabledChannelCount * m_ringConfig.alinesPerChannelPerBlock;
        const int blockSize = m_ringConfig.sampDepth * alines;
        m_ringAlineCount = alines;
        const int frameSize = nx * nx;
        m_ringBlockSize = blockSize;
        m_ringFrameSize = frameSize;
        size_t ringTotal = ringImagingShmTotalSize(blockSize, frameSize, alines);
        emit svcStatus(QString("[诊断] startSvc(RING): blockSize=%1 frameSize=%2 nx=%3")
                           .arg(blockSize).arg(frameSize).arg(nx), 0);
        emit svcStatus(QString("环形共享内存 %1 MB, blockSize=%2 frameSize=%3")
                           .arg(ringTotal / 1048576.0, 0, 'f', 1)
                           .arg(blockSize).arg(frameSize), 0);
        setupRingSharedMemory(blockSize, frameSize, alines);
    } else {
        emit svcStatus(QString("[诊断] startSvc: pulseSize=%1 frameSize=%2 nx=%3 ny=%4 depth=%5")
                       .arg(pulseSize).arg(frameSize)
                       .arg(m_scanParams.nx).arg(m_scanParams.ny)
                       .arg(m_generalParams.depth), 0);
        size_t totalSize = sizeof(ImagingShmHeader)
                           + static_cast<size_t>(pulseSize + frameSize) * sizeof(float);

        emit svcStatus(QString("共享内存 %1 MB, pulseSize=%2 frameSize=%3")
                           .arg(totalSize / 1048576.0, 0, 'f', 1)
                           .arg(pulseSize).arg(frameSize), 0);

        setupSharedMemory(pulseSize, frameSize);
    }

    // 创建 ZMQ PAIR 套接字（主进程作为 server bind）
    try {
        m_zmqCtx    = new zmq::context_t(1);
        m_zmqSocket = new zmq::socket_t(*m_zmqCtx, zmq::socket_type::pair);
        m_zmqSocket->bind(ZMQ_IPC_ENDPOINT);
    } catch (const zmq::error_t &e) {
        emit svcError(QString("ZMQ bind failed: %1").arg(e.what()));
        return false;
    }

    // ZMQ 轮询定时器（5ms 间隔，非阻塞收信令）
    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &ImagingController::pollControlMessages);
    m_pollTimer->start(5);

    // 启动 ImagingSvc 子进程
    m_svcProcess = new QProcess(this);

    // 查找子进程可执行文件
    QString svcPath = QCoreApplication::applicationDirPath() + "/ImagingSvc.exe";
    if (!QFile::exists(svcPath)) {
        svcPath = QCoreApplication::applicationDirPath() + "/../ImagingSvc/ImagingSvc.exe";
    }
    if (!QFile::exists(svcPath)) {
        emit svcError("找不到 ImagingSvc.exe");
        return false;
    }

    emit svcStatus(QString("启动 ImagingSvc: %1").arg(svcPath), 0);

    connect(m_svcProcess, &QProcess::started,
            this, &ImagingController::onSvcStarted);
    connect(m_svcProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ImagingController::onSvcFinished);
    connect(m_svcProcess, &QProcess::errorOccurred,
            this, &ImagingController::onSvcErrorOccurred);

    m_svcProcess->setProcessChannelMode(QProcess::ForwardedChannels);
    m_svcProcess->start(svcPath, QStringList());

    // 等待子进程就绪后下发配置和启动命令。
    // 300ms 在系统负载高时 ZMQ PAIR 连接可能尚未建立，configure 会丢失，
    // 加长到 1000ms 保证连接就绪（PAIR connect 前发送的消息会被丢弃）。
    QTimer::singleShot(1000, this, [this]() { sendConfigureAndStart(); });

    return true;
}

void ImagingController::stopSvc()
{
    if (!m_running) return;

    sendCommand({{"cmd", "stop"}});

    // 先停止 ZMQ 轮询，不再处理新消息
    if (m_pollTimer) {
        m_pollTimer->stop();
        delete m_pollTimer;
        m_pollTimer = nullptr;
    }

    // 异步停止子进程（非阻塞，避免 UI 卡顿）
    if (m_svcProcess && m_svcProcess->state() != QProcess::NotRunning) {
        m_svcProcess->terminate();
        // 单次超时：1.5s 后如果进程仍运行则强制 kill
        QTimer::singleShot(1500, this, [this]() {
            if (m_svcProcess && m_svcProcess->state() != QProcess::NotRunning) {
                m_svcProcess->kill();
                m_svcProcess->waitForFinished(500);
            }
            finishStopSvc();
        });
    } else {
        finishStopSvc();
    }
}

void ImagingController::finishStopSvc()
{
    delete m_zmqSocket;  m_zmqSocket = nullptr;
    delete m_zmqCtx;     m_zmqCtx    = nullptr;

    if (m_sharedMemory) {
        m_sharedMemory->detach();
        delete m_sharedMemory;
        m_sharedMemory = nullptr;
    }
    if (m_ringSharedMemory) {
        m_ringSharedMemory->detach();
        delete m_ringSharedMemory;
        m_ringSharedMemory = nullptr;
    }

    m_running = false;
    emit svcStopped();
}

bool ImagingController::isRunning() const
{
    return m_running
           && m_svcProcess
           && m_svcProcess->state() == QProcess::Running;
}

// =====================================================================
// 配置（startSvc 之前调用）
// =====================================================================
void ImagingController::configure(const GeneralParams &general,
                                  const ScanParams &scan,
                                  const ReconParams &recon)
{
    m_generalParams = general;
    m_scanParams    = scan;
    m_reconParams   = recon;
    m_frameWidth    = scan.nx;
    m_frameHeight   = scan.ny;

    emit svcStatus(QString("[诊断] configure: nx=%1 ny=%2 depth=%3 cardNum=%4 phyCh=%5")
                   .arg(scan.nx).arg(scan.ny)
                   .arg(general.depth).arg(general.cardNum)
                   .arg(general.physicalChannels), 0);

    // 每脉冲数据量 = 物理通道数 × depth（8 通道 × depth）
    int pulseSize = general.physicalChannels * general.depth;
    m_pulseAccumBuf.resize(pulseSize);
    m_pulseAccumBuf.fill(0.0f);
    m_pulseAccumCount = 0;
    m_pulseAccumSeq   = 0;
}

// =====================================================================
// 环形扫描并行分支：配置 / 块输入 / 共享内存
// =====================================================================
void ImagingController::configureRing(const RingReconCudaConfig &ringCfg)
{
    m_ringConfig = ringCfg;
    m_ringMode   = true;
    const int nx = static_cast<int>(std::ceil(m_ringConfig.fov / m_ringConfig.gridSize));
    m_ringAlineCount = m_ringConfig.enabledChannelCount * m_ringConfig.alinesPerChannelPerBlock;
    m_ringBlockSize = m_ringConfig.sampDepth * m_ringAlineCount;
    m_ringFrameSize = nx * nx;
    emit svcStatus(QString("[诊断] configureRing: sampDepth=%1 block=%2 nx=%3")
                       .arg(m_ringConfig.sampDepth)
                       .arg(m_ringConfig.alinesPerBlock)
                       .arg(nx), 0);
}

bool ImagingController::submitRingBlock(const QVector<float> &rawBlock,
                                  const QVector<float> &anglesDeg,
                                  const QVector<quint8> &channels,
                                  int blockSeq)
{
    if (rawBlock.size() != m_ringBlockSize) {
        emit svcError(QString("环形块大小错误：期望 %1，实际 %2")
                          .arg(m_ringBlockSize).arg(rawBlock.size()));
        return false;
    }

    m_ringSharedMemory->lock();
    auto *h = static_cast<RingImagingShmHeader *>(m_ringSharedMemory->data());
    auto *blockBuf = reinterpret_cast<float *>(h + 1);
    std::memcpy(blockBuf, rawBlock.constData(),
                static_cast<size_t>(m_ringBlockSize) * sizeof(float));
    auto *angleBuf = reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(h + 1) + ringAnglesOffset(m_ringBlockSize));
    auto *chanBuf = reinterpret_cast<uint8_t *>(reinterpret_cast<uint8_t *>(h + 1) + ringChannelsOffset(m_ringBlockSize, m_ringAlineCount));
    std::memcpy(angleBuf, anglesDeg.constData(), static_cast<size_t>(m_ringAlineCount) * sizeof(float));
    std::memcpy(chanBuf, channels.constData(), static_cast<size_t>(m_ringAlineCount));
    h->alines = static_cast<uint32_t>(m_ringAlineCount);
    h->block_seq   = static_cast<uint32_t>(blockSeq);
    h->block_ready = 1;
    m_ringSharedMemory->unlock();

    sendCommand({{"cmd", "ring_block_ready"}, {"seq", blockSeq}});
    return true;
}

void ImagingController::setupRingSharedMemory(int blockSize, int frameSize, int alines)
{
    if (m_ringSharedMemory) {
        m_ringSharedMemory->detach();
        delete m_ringSharedMemory;
    }

    m_ringSharedMemory = new QSharedMemory("MC_410T_RingShm", this);
    const size_t totalSize = ringImagingShmTotalSize(blockSize, frameSize, alines);
    if (!m_ringSharedMemory->create(static_cast<int>(totalSize))) {
        if (m_ringSharedMemory->error() == QSharedMemory::AlreadyExists) {
            m_ringSharedMemory->attach();
        }
        return;
    }

    m_ringSharedMemory->lock();
    std::memset(m_ringSharedMemory->data(), 0, totalSize);
    auto *h = static_cast<RingImagingShmHeader *>(m_ringSharedMemory->data());
    h->magic       = 0x52494E47;  // "RING"
    h->version     = 1;
    h->block_size  = static_cast<uint32_t>(blockSize);
    h->alines      = static_cast<uint32_t>(alines);
    h->frame_size  = static_cast<uint32_t>(frameSize);
    h->nx          = static_cast<uint16_t>(std::ceil(m_ringConfig.fov / m_ringConfig.gridSize));
    h->ny          = h->nx;
    m_ringSharedMemory->unlock();
}

void ImagingController::processRingMessage(const QJsonObject &msg)
{
    const QString cmd = msg["cmd"].toString();
    if (cmd != "ring_frame_ready" || !m_ringSharedMemory) return;

    const int seq = msg["seq"].toInt();
    const size_t frameSize = static_cast<size_t>(m_ringFrameSize);

    QVector<float> wl1(static_cast<int>(frameSize));
    QVector<float> wl2(static_cast<int>(frameSize));

    m_ringSharedMemory->lock();
    auto *h = static_cast<RingImagingShmHeader *>(m_ringSharedMemory->data());
    auto *fb = reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(h + 1) + ringFramesOffset(m_ringBlockSize, m_ringAlineCount));
    std::memcpy(wl1.data(), fb, frameSize * sizeof(float));
    std::memcpy(wl2.data(), fb + frameSize, frameSize * sizeof(float));
    h->frame_ready = 0;
    m_ringSharedMemory->unlock();

    const int nx = static_cast<int>(std::ceil(m_ringConfig.fov / m_ringConfig.gridSize));
    const int ny = nx;
    {
        QMutexLocker l(&m_ringFrameMutex);
        m_latestRingFrames[0] = wl1;
        m_latestRingFrames[1] = wl2;
        m_ringFrameNx = nx;
    }
    emit ringImageReady(frameDataToImage(wl1, nx, ny),
                        frameDataToImage(wl2, nx, ny), seq);
}

// =====================================================================
// 数据输入（MainWindow 调用，多卡聚合模式）
// 每张采集卡提供 2 个物理通道（freqA=通道A, freqB=通道B）
// 4 卡总计 8 通道，布局：
//   [卡0_A_depth, 卡0_B_depth, 卡1_A_depth, 卡1_B_depth,
//    卡2_A_depth, 卡2_B_depth, 卡3_A_depth, 卡3_B_depth]
// 由 flushPulseData 负责提交给子进程
// =====================================================================
void ImagingController::feedPulseData(int cardId, uint16_t triggerSeq,
                                       const QVector<float> &freqA,
                                       const QVector<float> &freqB)
{
    if (!m_running || !m_sharedMemory) return;

    int depth      = m_generalParams.depth;
    int sampleCnt  = qMin(freqA.size(), depth);

    // 物理通道 A：偏移 = cardId × 2 × depth
    int offsetA = cardId * 2 * depth;
    if (offsetA + sampleCnt <= m_pulseAccumBuf.size()) {
        std::memcpy(m_pulseAccumBuf.data() + offsetA,
                    freqA.constData(),
                    static_cast<size_t>(sampleCnt) * sizeof(float));
    }

    // 物理通道 B：偏移 = (cardId × 2 + 1) × depth
    if (!freqB.isEmpty()) {
        int sampleCntB = qMin(freqB.size(), depth);
        int offsetB = (cardId * 2 + 1) * depth;
        if (offsetB + sampleCntB <= m_pulseAccumBuf.size()) {
            std::memcpy(m_pulseAccumBuf.data() + offsetB,
                        freqB.constData(),
                        static_cast<size_t>(sampleCntB) * sizeof(float));
        }
    }

    m_pulseAccumCount++;

    // 用最后一张卡的 triggerSeq 作为脉冲序号
    m_pulseAccumSeq = triggerSeq;
}

// =====================================================================
// 刷新脉冲：将累积缓冲写入共享内存，通知子进程处理
// 由 MainWindow 在完成一轮多卡馈送后调用
// =====================================================================
void ImagingController::flushPulseData()
{
    if (!m_running || !m_sharedMemory) return;
    if (m_pulseAccumCount == 0) return;

    int pulseSize = m_generalParams.physicalChannels * m_generalParams.depth;

    m_sharedMemory->lock();
    auto *h = static_cast<ImagingShmHeader *>(m_sharedMemory->data());
    std::memcpy(reinterpret_cast<float *>(h + 1),
                m_pulseAccumBuf.constData(),
                static_cast<size_t>(pulseSize) * sizeof(float));
    h->pulse_seq   = static_cast<uint32_t>(m_pulseAccumSeq);
    h->pulse_ready = 1;
    m_sharedMemory->unlock();

    sendCommand({{"cmd", "pulse_ready"}, {"seq", m_pulseAccumSeq}});

    // 清空缓冲，准备下一脉冲
    m_pulseAccumBuf.fill(0.0f);
    m_pulseAccumCount = 0;
}

// =====================================================================
// 图像数据读取
// =====================================================================
QImage ImagingController::getLatestImage() const
{
    QMutexLocker locker(&m_frameMutex);
    if (m_latestFrame.isEmpty()) return QImage();
    return frameDataToImage(m_latestFrame, m_frameWidth, m_frameHeight);
}

QVector<float> ImagingController::getLatestFrameData() const
{
    QMutexLocker l(&m_frameMutex);
    return m_latestFrame;
}

int ImagingController::frameWidth() const  { return m_frameWidth; }
int ImagingController::frameHeight() const { return m_frameHeight; }

QVector<float> ImagingController::latestRingFrame(int index) const
{
    QMutexLocker l(&m_ringFrameMutex);
    if (index < 0 || index > 1) return {};
    return m_latestRingFrames[index];
}

int ImagingController::ringFrameNx() const
{
    QMutexLocker l(&m_ringFrameMutex);
    return m_ringFrameNx;
}

// =====================================================================
// Slot：子进程状态
// =====================================================================
void ImagingController::onSvcStarted()
{
    emit svcStatus(QString("ImagingSvc 进程已启动, PID=%1")
                       .arg(m_svcProcess ? m_svcProcess->processId() : 0), 0);
}

void ImagingController::onSvcFinished(int exitCode, QProcess::ExitStatus status)
{
    m_running = false;
    if (status == QProcess::CrashExit) {
        emit svcError(QString("ImagingSvc crashed (exit code %1)").arg(exitCode));
    }
}

void ImagingController::onSvcErrorOccurred(QProcess::ProcessError error)
{
    m_running = false;
    emit svcError(QString("ImagingSvc process error: %1").arg(error));
}

// =====================================================================
// ZMQ 消息轮询
// =====================================================================
void ImagingController::pollControlMessages()
{
    if (!m_zmqSocket) return;
    std::lock_guard<std::mutex> lk(m_zmqMutex);
    try {
        zmq::message_t msg;
        while (m_zmqSocket->recv(msg, zmq::recv_flags::dontwait)) {
            QByteArray data(static_cast<const char *>(msg.data()),
                            static_cast<int>(msg.size()));
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (doc.isObject()) processMessage(doc.object());
        }
    } catch (const zmq::error_t &e) {
        if (e.num() != EAGAIN) {
            qWarning() << "ImagingController ZMQ recv error:" << e.what();
        }
    }
}

// =====================================================================
// 信令处理
// =====================================================================
void ImagingController::processMessage(const QJsonObject &msg)
{
    QString cmd = msg["cmd"].toString();

    if (cmd == "ring_frame_ready") {
        processRingMessage(msg);
    } else if (cmd == "frame_ready") {
        int seq      = msg["seq"].toInt();
        int frameSize = m_scanParams.nx * m_scanParams.ny;

        m_sharedMemory->lock();
        auto *h  = static_cast<ImagingShmHeader *>(m_sharedMemory->data());
        int ps   = m_generalParams.physicalChannels * m_generalParams.depth;
        auto *fb = reinterpret_cast<float *>(
            reinterpret_cast<uint8_t *>(h + 1) + static_cast<size_t>(ps) * sizeof(float));

        QVector<float> frameData(frameSize);
        std::memcpy(frameData.data(), fb,
                    static_cast<size_t>(frameSize) * sizeof(float));
        h->frame_ready = 0;
        m_sharedMemory->unlock();

        {
            QMutexLocker l(&m_frameMutex);
            m_latestFrame = frameData;
        }

        emit imageReady(frameDataToImage(frameData, m_scanParams.nx, m_scanParams.ny), seq);

    } else if (cmd == "error") {
        emit svcError(msg["msg"].toString());

    } else if (cmd == "status") {
        emit svcStatus("running", static_cast<float>(msg["fps"].toDouble()));
    }
}

// =====================================================================
// ZMQ 发送命令
// =====================================================================
void ImagingController::sendCommand(const QJsonObject &cmd)
{
    if (!m_zmqSocket) return;
    std::lock_guard<std::mutex> lk(m_zmqMutex);

    QJsonDocument doc(cmd);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    zmq::message_t zmsg(static_cast<size_t>(data.size()));
    std::memcpy(zmsg.data(), data.constData(), static_cast<size_t>(data.size()));

    try {
        m_zmqSocket->send(zmsg, zmq::send_flags::dontwait);
    } catch (const zmq::error_t &) {
        // 非阻塞发送失败时静默处理
    }
}

// =====================================================================
// 下发配置 + 启动命令
// =====================================================================
void ImagingController::sendConfigureAndStart()
{
    if (!m_zmqSocket) return;

    m_running = true;
    emit svcReady();                   // 通知主窗口服务已就绪

    // ══ 环形扫描并行分支：下发完整可配置参数 ══
    if (m_ringMode) {
        QJsonObject ring;
        ring["sampDepth"]       = m_ringConfig.sampDepth;
        ring["reconDepth"]      = m_ringConfig.reconDepth;
        ring["alinesPerFrame"]  = m_ringConfig.alinesPerFrame;
        ring["alinesPerBlock"]  = m_ringConfig.alinesPerBlock;
        ring["rawColsPerBlock"] = m_ringConfig.rawColsPerBlock;
        ring["wlOffset"]        = m_ringConfig.wlOffset;
        ring["shiftWL2"]        = m_ringConfig.shiftWL2;
        ring["reconMode"]       = m_ringConfig.reconMode;
        ring["readMode"]        = m_ringConfig.readMode;
        ring["daqHz"]           = m_ringConfig.daqHz;
        ring["radius"]          = m_ringConfig.radius;
        ring["alineRateHz"]     = m_ringConfig.alineRateHz;
        QJsonArray speeds;
        for (int i = 0; i < m_ringConfig.soundSpeedsCount && i < 8; ++i)
            speeds.append(m_ringConfig.soundSpeeds[i]);
        ring["soundSpeeds"]     = speeds;
        ring["fov"]             = m_ringConfig.fov;
        ring["gridSize"]        = m_ringConfig.gridSize;
        ring["coverageDeg"]     = m_ringConfig.coverageDeg;
        ring["theta0Deg"]       = m_ringConfig.theta0Deg;
        ring["fovDeg"]          = m_ringConfig.fovDeg;
        ring["fovTheta0Deg"]    = m_ringConfig.fovTheta0Deg;
        ring["apodType"]        = m_ringConfig.apodType;
        ring["distanceWeightExponent"] = m_ringConfig.distanceWeightExponent;
        ring["interpolation"]   = m_ringConfig.interpolation;
        ring["minDistance"]     = m_ringConfig.minDistance;
        ring["maskOutOfRange"]  = m_ringConfig.maskOutOfRange;
        ring["dbrSigRemove"]    = m_ringConfig.dbrSigRemove;
        ring["maskLength"]      = m_ringConfig.maskLength;
        ring["delayCut"]        = m_ringConfig.delayCut;
        ring["singalImpair"]    = m_ringConfig.singalImpair;
        QJsonArray imv;
        imv.append(m_ringConfig.imValue[0]);
        imv.append(m_ringConfig.imValue[1]);
        ring["imValue"]         = imv;
        QJsonArray sd;
        sd.append(m_ringConfig.sysDelay[0]);
        sd.append(m_ringConfig.sysDelay[1]);
        ring["sysDelay"]        = sd;
        ring["refCol"]          = m_ringConfig.refCol;
        QJsonArray cr;
        cr.append(m_ringConfig.corrows[0]);
        cr.append(m_ringConfig.corrows[1]);
        ring["corrows"]         = cr;
        ring["interpFactor"]    = m_ringConfig.interpFactor;
        QJsonArray chArr;
        for (int i = 0; i < 8; ++i) chArr.append(m_ringConfig.enabledChannels[i]);
        ring["enabledChannels"] = chArr;
        ring["enabledChannelCount"] = m_ringConfig.enabledChannelCount;
        ring["alinesPerChannelPerBlock"] = m_ringConfig.alinesPerChannelPerBlock;
        ring["alinesPerChannelPerFrame"] = m_ringConfig.alinesPerChannelPerFrame;
        ring["sectorStartDeg"] = m_ringConfig.sectorStartDeg;
        ring["sectorCcw"] = m_ringConfig.sectorCcw;
        ring["triggerWlOdd"] = m_ringConfig.triggerWlOdd;

                QJsonObject params;
        params["imagingMode"] = "ring";
        params["ring"] = ring;
        sendCommand({{"cmd", "configure"}, {"params", params}});
        QThread::msleep(10);
        sendCommand({{"cmd", "start"}});
        return;
    }

    // ══ 声速参数发送前保护 ════════════════════════════════════════
    if (m_reconParams.sos1_mps <= 0) m_reconParams.sos1_mps = 1500.0f;
    if (m_reconParams.sos2_mps <= 0) m_reconParams.sos2_mps = m_reconParams.sos1_mps;

    // 通用参数
    QJsonObject g;
    g["daq_hz"]         = static_cast<double>(m_generalParams.daq_hz);
    g["depth"]          = m_generalParams.depth;
    g["channelNum"]     = m_generalParams.channelNum;
    g["cardNum"]        = m_generalParams.cardNum;
    g["physicalChannels"] = m_generalParams.physicalChannels;
    g["isMultiFiber"]   = m_generalParams.isMultiFiber;

    // 扫描参数
    QJsonObject s;
    s["stepsize_um"]  = static_cast<double>(m_scanParams.stepsize_um);
    s["move_aline"]   = m_scanParams.move_aline;
    s["channel_aline"] = m_scanParams.channel_aline;
    s["nx"]           = m_scanParams.nx;
    s["ny"]           = m_scanParams.ny;
    s["dx_um"]        = static_cast<double>(m_scanParams.dx_um);
    s["dy_um"]        = static_cast<double>(m_scanParams.dy_um);
    s["x0_m"]         = static_cast<double>(m_scanParams.x0_m);
    s["y0_m"]         = static_cast<double>(m_scanParams.y0_m);

    // 重建参数
    QJsonObject r;
    r["delay"]              = m_reconParams.delay;
    r["sos1_mps"]           = static_cast<double>(m_reconParams.sos1_mps);
    r["sos2_mps"]           = static_cast<double>(m_reconParams.sos2_mps);
    r["mediumLinePos_m"]    = static_cast<double>(m_reconParams.mediumLinePos_m);
    r["isDualSoS"]          = m_reconParams.isDualSoS;
    r["filterType"]         = m_reconParams.filterType;
    r["filterFreqLow_Hz"]   = static_cast<double>(m_reconParams.filterFreqLow_Hz);
    r["filterFreqHigh_Hz"]  = static_cast<double>(m_reconParams.filterFreqHigh_Hz);
    r["threshold"]          = static_cast<double>(m_reconParams.threshold);
    r["dynRange_db"]        = static_cast<double>(m_reconParams.dynRange_db);
    r["outputType"]         = m_reconParams.outputType;
    r["isReadFiberPosition"] = m_reconParams.isReadFiberPosition;
    r["delayTimePoint"]     = m_reconParams.delayTimePoint;
    r["s1Period"]           = m_reconParams.s1Period;
    r["isCutoffLoc"]        = m_reconParams.isCutoffLoc;
    r["cutoffLoc"]          = m_reconParams.cutoffLoc;
    r["isCenterAlign"]      = m_reconParams.isCenterAlign;

    // 配准数据（QVector<int> → QJsonArray）
    QJsonArray xArr, yArr, caliArr, fiberArr;
    for (int v : m_reconParams.xPeizhun)       xArr.append(v);
    for (int v : m_reconParams.yPeizhun)       yArr.append(v);
    for (int v : m_reconParams.caliCardDelay)   caliArr.append(v);
    for (int v : m_reconParams.caliFiberDelay)  fiberArr.append(v);
    r["xPeizhun"]       = xArr;
    r["yPeizhun"]       = yArr;
    r["caliCardDelay"]  = caliArr;
    r["caliFiberDelay"] = fiberArr;

    QJsonObject params;
    params["general"] = g;
    params["scan"]    = s;
    params["recon"]   = r;

    sendCommand({{"cmd", "configure"}, {"params", params}});
    QThread::msleep(10);
    sendCommand({{"cmd", "start"}});
}

// =====================================================================
// 共享内存创建
// =====================================================================
void ImagingController::setupSharedMemory(int pulseSize, int frameSize)
{
    if (m_sharedMemory) {
        m_sharedMemory->detach();
        delete m_sharedMemory;
    }

    m_sharedMemory = new QSharedMemory("MC_410T_ImagingShm", this);

    // 若已存在则尝试 attach，否则 create
    if (!m_sharedMemory->create(imagingShmTotalSize(pulseSize, frameSize))) {
        if (m_sharedMemory->error() == QSharedMemory::AlreadyExists) {
            m_sharedMemory->attach();
        }
        return;
    }

    // 初始化 header
    m_sharedMemory->lock();
    std::memset(m_sharedMemory->data(), 0,
                imagingShmTotalSize(pulseSize, frameSize));
    auto *h = static_cast<ImagingShmHeader *>(m_sharedMemory->data());
    h->magic       = 0x494D4147;  // "IMAG"
    h->version     = 1;
    h->pulse_size  = static_cast<uint32_t>(pulseSize);
    h->frame_size  = static_cast<uint32_t>(frameSize);
    h->nx          = static_cast<uint16_t>(m_scanParams.nx);
    h->ny          = static_cast<uint16_t>(m_scanParams.ny);
    m_sharedMemory->unlock();
}

// =====================================================================
// 帧数据 → QImage 转换（灰度映射）
// =====================================================================
QImage ImagingController::frameDataToImage(const QVector<float> &data,
                                            int nx, int ny) const
{
    if (data.size() < nx * ny) return QImage();

    // 找极值
    float minV = 1e30f, maxV = -1e30f;
    for (int i = 0; i < nx * ny; ++i) {
        float v = data[i];
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
    }

    float range = maxV - minV;
    if (range < 1e-6f) range = 1.0f;

    QImage img(nx, ny, QImage::Format_Grayscale8);

    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            float t = (data[y * nx + x] - minV) / range;
            t = qBound(0.0f, t, 1.0f);
            const int g = static_cast<int>(t * 255.0f);
            img.setPixelColor(x, ny - 1 - y, QColor(g, g, g));
        }
    }

    return img;
}
