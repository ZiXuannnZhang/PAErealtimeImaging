#include "ImagingSvc.h"
#include "ImagingSharedMemory.h"
#include "pa_recon_qt.hpp"
#include "ring_recon.h"
#include <zmq.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <cmath>
#include <cstring>

// ZMQ IPC 端点（与主进程 ImagingController 约定一致）
static const char *ZMQ_IPC_ENDPOINT = "tcp://127.0.0.1:5555";

// =====================================================================
// 构造 / 析构
// =====================================================================
ImagingSvc::ImagingSvc(QObject *parent)
    : QObject(parent)
    , m_reconstructor(nullptr)
    , m_sharedMemory(nullptr)
    , m_zmqCtx(nullptr)
    , m_zmqSocket(nullptr)
    , m_pollTimer(nullptr)
    , m_pulseCount(0)
    , m_initialized(false)
    , m_running(false)
    , m_ringSharedMemory(nullptr)
{
}

ImagingSvc::~ImagingSvc()
{
    m_running = false;
    delete m_reconstructor;
    for (int w = 0; w < 2; ++w) {
        if (m_ringCuda[w]) {
            ring_recon_cuda_destroy(m_ringCuda[w]);
            m_ringCuda[w] = nullptr;
        }
    }
    if (m_ringSharedMemory) {
        m_ringSharedMemory->detach();
        delete m_ringSharedMemory;
        m_ringSharedMemory = nullptr;
    }

    if (m_pollTimer) {
        m_pollTimer->stop();
        delete m_pollTimer;
    }

    delete m_zmqSocket;
    delete m_zmqCtx;
}

// =====================================================================
// 初始化：连接 ZMQ + 附接共享内存
// =====================================================================
bool ImagingSvc::initialize()
{
    // 连接 ZMQ PAIR（作为 client connect）
    try {
        m_zmqCtx    = new zmq::context_t(1);
        m_zmqSocket = new zmq::socket_t(*m_zmqCtx, zmq::socket_type::pair);
        m_zmqSocket->connect(ZMQ_IPC_ENDPOINT);
    } catch (const zmq::error_t &e) {
        qCritical() << "ImagingSvc: ZMQ connect failed:" << e.what();
        return false;
    }

    // ZMQ 轮询定时器
    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout,
            this, &ImagingSvc::pollControlMessages);
    m_pollTimer->start(5);

    // 附接共享内存（线性模式必有；环形模式由 processRingConfigure 单独附接）
    m_sharedMemory = new QSharedMemory("MC_410T_ImagingShm", this);
    if (!m_sharedMemory->attach()) {
        qWarning() << "ImagingSvc: linear shared memory not present (ring mode?)";
    }

    m_initialized = true;
    return true;
}

// =====================================================================
// ZMQ 消息轮询
// =====================================================================
void ImagingSvc::pollControlMessages()
{
    if (!m_zmqSocket) return;

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
            qWarning() << "ImagingSvc ZMQ recv error:" << e.what();
        }
    }
}

// =====================================================================
// 信令分发
// =====================================================================
void ImagingSvc::processMessage(const QJsonObject &msg)
{
    QString cmd = msg["cmd"].toString();

    if (cmd == "configure") {
        processConfigure(msg["params"].toObject());
    } else if (cmd == "ring_block_ready") {
        processRingPulse();
    } else if (cmd == "ring_reset") {
        // 停机超时判定新一圈：清空重建累积（与圈末重置同一函数）
        resetRingRecon();
    } else if (cmd == "start") {
        m_running = true;
        m_pulseCount = 0;
        m_ringBlockIndex = 0;
        for (int c = 0; c < 8; ++c) {
            m_ringPrevWL2[c].clear();
            m_ringPrevAngle[c] = 0.0f;
            m_ringPrevRadius[c] = 0.0f;
        }
    } else if (cmd == "stop") {
        m_running = false;
    } else if (cmd == "pulse_ready") {
        processPulse();
    }
}

// =====================================================================
// 配置处理：解析 JSON → 创建 PAReconstructor
// =====================================================================
void ImagingSvc::processConfigure(const QJsonObject &params)
{
    // 环形扫描并行分支：与线性 pa_recon 完全独立
    if (params["imagingMode"].toString() == "ring") {
        processRingConfigure(params["ring"].toObject());
        return;
    }

    QJsonObject g = params["general"].toObject();
    m_generalParams.daq_hz     = static_cast<float>(g["daq_hz"].toDouble());
    m_generalParams.depth      = g["depth"].toInt();
    m_generalParams.channelNum = g["channelNum"].toInt();
    m_generalParams.cardNum    = g["cardNum"].toInt();
    m_generalParams.physicalChannels = g["physicalChannels"].toInt(8);
    m_generalParams.isMultiFiber = g["isMultiFiber"].toBool();
    m_generalParams.channelNum = g["channelNum"].toInt();

    QJsonObject s = params["scan"].toObject();
    m_scanParams.stepsize_um  = static_cast<float>(s["stepsize_um"].toDouble());
    m_scanParams.move_aline   = s["move_aline"].toInt();
    m_scanParams.channel_aline = s["channel_aline"].toInt();
    m_scanParams.nx           = s["nx"].toInt();
    m_scanParams.ny           = s["ny"].toInt();
    m_scanParams.dx_um        = static_cast<float>(s["dx_um"].toDouble());
    m_scanParams.dy_um        = static_cast<float>(s["dy_um"].toDouble());
    m_scanParams.x0_m         = static_cast<float>(s["x0_m"].toDouble());
    m_scanParams.y0_m         = static_cast<float>(s["y0_m"].toDouble());

    QJsonObject r = params["recon"].toObject();
    m_reconParams.delay                = r["delay"].toInt();
    m_reconParams.sos1_mps             = static_cast<float>(r["sos1_mps"].toDouble());
    m_reconParams.sos2_mps             = static_cast<float>(r["sos2_mps"].toDouble());
    m_reconParams.mediumLinePos_m      = static_cast<float>(r["mediumLinePos_m"].toDouble());
    m_reconParams.isDualSoS            = r["isDualSoS"].toBool();
    m_reconParams.filterType           = r["filterType"].toString();
    m_reconParams.filterFreqLow_Hz     = static_cast<float>(r["filterFreqLow_Hz"].toDouble());
    m_reconParams.filterFreqHigh_Hz    = static_cast<float>(r["filterFreqHigh_Hz"].toDouble());
    m_reconParams.threshold            = static_cast<float>(r["threshold"].toDouble());
    m_reconParams.dynRange_db          = static_cast<float>(r["dynRange_db"].toDouble());
    m_reconParams.outputType           = r["outputType"].toString();
    m_reconParams.isReadFiberPosition  = r["isReadFiberPosition"].toBool();
    m_reconParams.delayTimePoint       = r["delayTimePoint"].toInt(1601);
    m_reconParams.s1Period             = r["s1Period"].toInt(2500);
    m_reconParams.isCutoffLoc          = r["isCutoffLoc"].toBool(false);
    m_reconParams.cutoffLoc            = r["cutoffLoc"].toInt(2501);
    m_reconParams.isCenterAlign        = r["isCenterAlign"].toBool(true);

    // 解析配准数据（QJsonArray → QVector<int>）
    auto jsonArrToVec = [](const QJsonArray &arr) {
        QVector<int> v;
        v.reserve(arr.size());
        for (const auto &val : arr) v.append(val.toInt());
        return v;
    };
    m_reconParams.xPeizhun       = jsonArrToVec(r["xPeizhun"].toArray());
    m_reconParams.yPeizhun       = jsonArrToVec(r["yPeizhun"].toArray());
    m_reconParams.caliCardDelay  = jsonArrToVec(r["caliCardDelay"].toArray());
    m_reconParams.caliFiberDelay = jsonArrToVec(r["caliFiberDelay"].toArray());

    // 构造 pa_recon_qt::Config
    pa_recon_qt::Config config;
    config.depth        = m_generalParams.depth;
    config.channel_num  = m_generalParams.channelNum;
    config.card_num     = m_generalParams.cardNum;
    config.move_aline   = m_scanParams.move_aline;
    config.channel_aline = m_scanParams.channel_aline;
    config.nx           = m_scanParams.nx;
    config.ny           = m_scanParams.ny;
    config.fs           = m_generalParams.daq_hz;
    config.x0           = m_scanParams.x0_m;
    config.y0           = m_scanParams.y0_m;
    config.dx           = m_scanParams.dx_um * 1e-6f;
    config.dy           = m_scanParams.dy_um * 1e-6f;
    config.detx_step    = m_scanParams.stepsize_um * 1e-6f;
    config.delay        = m_reconParams.delay;
    config.threhold     = m_reconParams.threshold;
    config.use_dual     = m_reconParams.isDualSoS;
    config.boundary_y   = m_reconParams.mediumLinePos_m;
    config.lower_sos    = m_reconParams.sos1_mps;
    config.upper_sos    = m_reconParams.sos2_mps;
    config.dyn_range_db = m_reconParams.dynRange_db;
    config.is_read_fiber_position = m_reconParams.isReadFiberPosition;
    config.delay_time_point      = m_reconParams.delayTimePoint;
    config.s1period              = m_reconParams.s1Period;
    config.is_center_align       = m_reconParams.isCenterAlign;

    // 滤波器配置
    if (m_reconParams.filterType == "Bandpass") {
        config.filter_type  = pa_recon_qt::Config::FilterType::Bandpass;
        config.bandpass_hz  = {m_reconParams.filterFreqLow_Hz,
                               m_reconParams.filterFreqHigh_Hz};
        config.filter_cut   = {static_cast<double>(m_reconParams.filterFreqLow_Hz),
                               static_cast<double>(m_reconParams.filterFreqHigh_Hz)};
        config.filter_order = 4;
    } else if (m_reconParams.filterType == "Highpass") {
        config.filter_type  = pa_recon_qt::Config::FilterType::Highpass;
        config.filter_cut   = {static_cast<double>(m_reconParams.filterFreqLow_Hz)};
        config.filter_order = 4;
    } else if (m_reconParams.filterType == "Lowpass") {
        config.filter_type  = pa_recon_qt::Config::FilterType::Lowpass;
        config.filter_cut   = {static_cast<double>(m_reconParams.filterFreqHigh_Hz)};
        config.filter_order = 4;
    } else {
        config.filter_type  = pa_recon_qt::Config::FilterType::None;
    }

    // 输出类型
    if (m_reconParams.outputType == "RF") {
        config.output_type = pa_recon_qt::Config::OutputType::RF;
    } else if (m_reconParams.outputType == "ENV") {
        config.output_type = pa_recon_qt::Config::OutputType::ENV;
    } else {
        config.output_type = pa_recon_qt::Config::OutputType::DB;
    }

    // 光纤类型
    config.fiber_type = m_generalParams.isMultiFiber
                            ? pa_recon_qt::Config::FiberType::F64
                            : pa_recon_qt::Config::FiberType::F8;

    // F8 模式覆写拼接与校准参数（匹配 stl_f8_example 250M v3）
    if (config.fiber_type == pa_recon_qt::Config::FiberType::F8) {
        config.delay_time_point = 4501;                     // MATLAB v3: delayTimePoint = 4501
        config.s1period = 4000;                             // MATLAB v3: TotalDepth = 4000
        config.CaliCardDelay = {0, 2, 2, 4, 8, 11, 12, 14}; // 250M v3 专用
        config.CaliFiberDelay = {534, 484, 428, 372, 322, 264, 282, 202};
        config.is_center_align = true;
    }

    qDebug() << "ImagingSvc Config: depth=" << config.depth
             << "card_num=" << config.card_num
             << "channel_num=" << config.channel_num
             << "move_aline=" << config.move_aline
             << "channel_aline=" << config.channel_aline
             << "nx=" << config.nx << "ny=" << config.ny
             << "delay=" << config.delay
             << "delay_time_point=" << config.delay_time_point
             << "s1period=" << config.s1period
             << "fiber_type=" << (config.fiber_type == pa_recon_qt::Config::FiberType::F8 ? "F8" : "F64");

    // card_num = 物理通道数（每脉冲 8 个数据块）
    config.card_num   = m_generalParams.physicalChannels;
    config.channel_num = m_generalParams.channelNum;

    // 应用从配准文件加载的数据（若存在且长度匹配）
    if (!m_reconParams.xPeizhun.isEmpty() &&
        m_reconParams.xPeizhun.size() == config.channel_num) {
        config.x_peizhun = m_reconParams.xPeizhun;
        config.y_peizhun = m_reconParams.yPeizhun;
    }
    if (!m_reconParams.caliCardDelay.isEmpty() &&
        m_reconParams.caliCardDelay.size() == config.channel_num) {
        config.CaliCardDelay = m_reconParams.caliCardDelay;
    }
    if (!m_reconParams.caliFiberDelay.isEmpty() &&
        m_reconParams.caliFiberDelay.size() == config.channel_num) {
        config.CaliFiberDelay = m_reconParams.caliFiberDelay;
    }

    // 末尾干扰段衰减
    config.is_cutoff_loc = m_reconParams.isCutoffLoc ? 1 : 0;
    config.cutoff_point  = m_reconParams.cutoffLoc;

    // 配准数据长度必须匹配 channel_num，否则算法拒绝创建
    // 不匹配时清空配准数据并关闭 is_read_fiber_position
    if (config.x_peizhun.size() != config.channel_num) {
        config.x_peizhun.clear();
        config.y_peizhun.clear();
        config.is_read_fiber_position = false;
    }

    // ══ 声速参数兜底保护（C 库无条件要求 lower_sos > 0）══════════
    if (config.lower_sos <= 0) config.lower_sos = 1500.0f;
    if (config.upper_sos <= 0) config.upper_sos = config.lower_sos;

    // ══ 诊断日志：输出的 config 值 ══════════════════════════════
    qDebug() << "ImagingSvc Config final: use_dual=" << config.use_dual
             << "lower_sos=" << config.lower_sos
             << "upper_sos=" << config.upper_sos
             << "boundary_y=" << config.boundary_y
             << "fiber_type=" << (config.fiber_type == pa_recon_qt::Config::FiberType::F8 ? "F8" : "F64");

    // 创建 PAReconstructor
    delete m_reconstructor;
    m_reconstructor = nullptr;

    try {
        sendStatus(0.0f);
        // 先构造局部对象，捕获异常后仍可读取 C 结构体诊断值
        pa_recon_qt::PAReconstructor *r = new pa_recon_qt::PAReconstructor(config);
        m_reconstructor = r;
    } catch (const std::exception &e) {
        // 诊断：输出 config 值帮助定位
        QString diag = QString("  [use_dual=%1 lower=%2 upper=%3]")
                       .arg(config.use_dual ? "1" : "0")
                       .arg(config.lower_sos, 0, 'f', 1)
                       .arg(config.upper_sos, 0, 'f', 1);
        sendError(QString("Failed to create PAReconstructor: %1  %2")
                  .arg(e.what()).arg(diag), 1001);
        return;
    }

    // 每脉冲数据量 = physicalChannels × depth
    m_pulseBuffer.resize(m_generalParams.physicalChannels * m_generalParams.depth);
    m_pulseCount = 0;
}

// =====================================================================
// 环形扫描并行分支：配置 / 分块重建 / 双帧回传
// =====================================================================
void ImagingSvc::processRingConfigure(const QJsonObject &ring)
{
    RingReconCudaConfig cfg;
    ring_recon_cuda_set_defaults(&cfg);

    cfg.sampDepth       = ring["sampDepth"].toInt(cfg.sampDepth);
    cfg.reconDepth      = ring["reconDepth"].toInt(cfg.reconDepth);
    cfg.alinesPerFrame  = ring["alinesPerFrame"].toInt(cfg.alinesPerFrame);
    cfg.alinesPerBlock  = ring["alinesPerBlock"].toInt(cfg.alinesPerBlock);
    cfg.shiftWL2        = ring["shiftWL2"].toInt(cfg.shiftWL2);
    cfg.daqHz           = ring["daqHz"].toDouble(cfg.daqHz);
    cfg.radius          = ring["radius"].toDouble(cfg.radius);
    cfg.multiRadius     = ring["multiRadius"].toInt(cfg.multiRadius);
    cfg.spliceMode      = ring["spliceMode"].toInt(cfg.spliceMode);
    cfg.spliceBlendDeg  = ring["spliceBlendDeg"].toDouble(cfg.spliceBlendDeg);
    const QJsonArray radiusCh = ring["radiusPerChannel"].toArray();
    for (int i = 0; i < 8; ++i) {
        if (i < radiusCh.size() && radiusCh[i].toDouble() > 0.0)
            cfg.radiusPerChannel[i] = radiusCh[i].toDouble();
    }
    if (!cfg.multiRadius) {
        for (int i = 0; i < 8; ++i)
            cfg.radiusPerChannel[i] = cfg.radius;
        cfg.spliceMode = 0;   // 拼接模式仅在配准模式下有效
    }

    const QJsonArray speeds = ring["soundSpeeds"].toArray();
    if (!speeds.isEmpty()) {
        cfg.soundSpeedsCount = qMin(speeds.size(), 8);
        for (int i = 0; i < cfg.soundSpeedsCount; ++i)
            cfg.soundSpeeds[i] = speeds[i].toDouble();
    }
    // 双声速分层模型：边界半径 [m]（空=单一声速），各层声速元素数=边界数+1
    cfg.soundSpeedRadiiCount = qBound(0, ring["soundSpeedRadiiCount"].toInt(cfg.soundSpeedRadiiCount), 8);
    const QJsonArray radii = ring["soundSpeedRadii"].toArray();
    if (cfg.soundSpeedRadiiCount > 0 && radii.size() >= cfg.soundSpeedRadiiCount) {
        for (int i = 0; i < cfg.soundSpeedRadiiCount; ++i)
            cfg.soundSpeedRadii[i] = radii[i].toDouble();
    } else {
        cfg.soundSpeedRadiiCount = 0;
    }

    cfg.fov                    = ring["fov"].toDouble(cfg.fov);
    cfg.gridSize               = ring["gridSize"].toDouble(cfg.gridSize);
    cfg.fovDeg                 = ring["fovDeg"].toDouble(cfg.fovDeg);
    cfg.fovTheta0Deg           = ring["fovTheta0Deg"].toDouble(cfg.fovTheta0Deg);
    cfg.apodType               = ring["apodType"].toInt(cfg.apodType);
    cfg.distanceWeightExponent = ring["distanceWeightExponent"].toDouble(cfg.distanceWeightExponent);
    cfg.interpolation          = ring["interpolation"].toInt(cfg.interpolation);
    cfg.minDistance            = ring["minDistance"].toDouble(cfg.minDistance);
    cfg.maskOutOfRange         = ring["maskOutOfRange"].toInt(cfg.maskOutOfRange);
    cfg.dbrSigRemove           = ring["dbrSigRemove"].toInt(cfg.dbrSigRemove);
    cfg.maskLength             = ring["maskLength"].toInt(cfg.maskLength);
    cfg.delayCut               = ring["delayCut"].toInt(cfg.delayCut);
    cfg.singalImpair           = ring["singalImpair"].toInt(cfg.singalImpair);

    const QJsonArray imv = ring["imValue"].toArray();
    if (imv.size() >= 2) {
        cfg.imValue[0] = imv[0].toDouble();
        cfg.imValue[1] = imv[1].toDouble();
    }
    const QJsonArray sd = ring["sysDelay"].toArray();
    if (sd.size() >= 2) {
        cfg.sysDelay[0] = sd[0].toInt();
        cfg.sysDelay[1] = sd[1].toInt();
    }
    // 每通道双波长延时截断：优先新协议 sysDelayPerChannel[8][2]；
    // 未提供/字段不完整时回退为 cfg.sysDelay（旧全局值）广播到所有通道，保持旧客户端行为不变。
    for (int c = 0; c < 8; ++c) {
        m_ringSysDelayCh[c][0] = cfg.sysDelay[0];
        m_ringSysDelayCh[c][1] = cfg.sysDelay[1];
    }
    const QJsonArray sdCh = ring["sysDelayPerChannel"].toArray();
    if (sdCh.size() >= 8 && sdCh[0].isArray()) {
        for (int c = 0; c < 8; ++c) {
            const QJsonArray pair = sdCh[c].toArray();
            if (pair.size() >= 2) {
                m_ringSysDelayCh[c][0] = pair[0].toInt();
                m_ringSysDelayCh[c][1] = pair[1].toInt();
            }
        }
    }

    // ---- 8 通道扇区模型 ----
    const QJsonArray chArr = ring["enabledChannels"].toArray();
    int cnt = 0;
    for (int i = 0; i < 8; ++i) {
        cfg.enabledChannels[i] = (i < chArr.size() && chArr[i].toInt()) ? 1 : 0;
        if (cfg.enabledChannels[i]) ++cnt;
    }
    if (cnt == 0) {
        sendError("未勾选任何通道", 2004);
        return;
    }
    cfg.enabledChannelCount = cnt;
    // 拼接羽化宽度：仅拼接模式有效，并 clamp 到小于半扇区宽（避免整扇区羽化）
    if (!cfg.spliceMode) {
        cfg.spliceBlendDeg = 0.0;
    } else {
        const double maxBlend = 0.5 * 360.0 / static_cast<double>(cnt) - 0.01;
        cfg.spliceBlendDeg = qBound(0.0, cfg.spliceBlendDeg, maxBlend);
    }
    cfg.alinesPerChannelPerBlock = ring["alinesPerChannelPerBlock"].toInt(cfg.alinesPerChannelPerBlock);
    cfg.alinesPerChannelPerFrame = ring["alinesPerChannelPerFrame"].toInt(cfg.alinesPerChannelPerFrame);
    cfg.sectorStartDeg = ring["sectorStartDeg"].toDouble(cfg.sectorStartDeg);
    cfg.sectorCcw = ring["sectorCcw"].toInt(cfg.sectorCcw);
    cfg.triggerWlOdd = ring["triggerWlOdd"].toInt(cfg.triggerWlOdd);
    cfg.timeoutResetSec = ring["timeoutResetSec"].toDouble(cfg.timeoutResetSec);
    if (cfg.alinesPerChannelPerBlock <= 0 || cfg.alinesPerChannelPerBlock % 2 != 0) {
        sendError("组包列数必须为正偶数", 2005);
        return;
    }

    m_ringConfig = cfg;
    m_ringMode   = true;
    m_ringChannelCount = cnt;
    for (int i = 0; i < 8; ++i) m_ringChannels[i] = cfg.enabledChannels[i];
    m_ringAlineCount = cnt * cfg.alinesPerChannelPerBlock;
    m_ringBlockSize = cfg.sampDepth * m_ringAlineCount;
    const int nx = static_cast<int>(std::ceil(cfg.fov / cfg.gridSize));
    m_ringFrameSize = nx * nx;
    // 方案A：显示图像=全分辨率重建矩阵（每像素=gridSize），不做显示级降采样
    m_ringDisplayNx = nx;
    m_ringDisplayStep = 1;
    m_ringDisplayFrameSize = nx * nx;
    {
        const int blocks = (cnt > 0 && cfg.alinesPerChannelPerBlock > 0)
            ? cfg.alinesPerFrame / (cnt * cfg.alinesPerChannelPerBlock) : 0;
        m_ringBlocksPerFrame = blocks > 0 ? blocks : 1;
    }
    m_ringDisplay0.assign(static_cast<size_t>(m_ringFrameSize), 0.0f);
    m_ringDisplay1.assign(static_cast<size_t>(m_ringFrameSize), 0.0f);
    m_ringBlockIndex = 0;
    for (int c = 0; c < 8; ++c) {
        m_ringPrevWL2[c].clear();
        m_ringPrevAngle[c] = 0.0f;
        m_ringPrevRadius[c] = 0.0f;
    }

    for (int w = 0; w < 2; ++w) {
        if (m_ringCuda[w]) {
            ring_recon_cuda_destroy(m_ringCuda[w]);
            m_ringCuda[w] = nullptr;
        }
        if (ring_recon_cuda_create(&cfg, &m_ringCuda[w]) != 0) {
            sendError(QString("ring_recon_cuda_create failed: %1")
                          .arg(QString::fromUtf8(ring_recon_cuda_last_error())), 2006);
            return;
        }
    }

    if (m_ringSharedMemory) {
        m_ringSharedMemory->detach();
        delete m_ringSharedMemory;
        m_ringSharedMemory = nullptr;
    }
    m_ringSharedMemory = new QSharedMemory("MC_410T_RingShm", this);
    if (!m_ringSharedMemory->attach()) {
        sendError("环形共享内存附接失败", 2007);
        return;
    }
    // v2 强校验：显示参数与 SHM 头一致，避免尺寸漂移导致越界读写
    m_ringSharedMemory->lock();
    auto *hdr = static_cast<RingImagingShmHeader *>(m_ringSharedMemory->data());
    const bool shmOk = hdr
        && hdr->magic == 0x52494E47u
        && hdr->version == 2u
        && hdr->frame_size == static_cast<uint32_t>(m_ringFrameSize)
        && hdr->nx == static_cast<uint16_t>(nx)
        && hdr->display_nx == static_cast<uint16_t>(m_ringDisplayNx)
        && hdr->display_frame_size == static_cast<uint32_t>(m_ringDisplayFrameSize)
        && hdr->snapshot_step == static_cast<uint32_t>(m_ringDisplayStep);
    m_ringSharedMemory->unlock();
    if (!shmOk) {
        sendError("环形共享内存版本/显示参数不匹配（需要 v2 布局）", 2010);
        return;
    }

    qDebug() << "ImagingSvc ring mode: channels=" << cnt
             << "perChannelBlock=" << cfg.alinesPerChannelPerBlock
             << "blockSize=" << m_ringBlockSize
             << "frameSize=" << m_ringFrameSize << "nx=" << nx;
}

void ImagingSvc::processRingPulse()
{
    if (!m_running || !m_ringCuda[0] || !m_ringCuda[1] || !m_ringSharedMemory) return;

    m_ringSharedMemory->lock();
    auto *h = static_cast<RingImagingShmHeader *>(m_ringSharedMemory->data());
    const int blockSize = m_ringBlockSize;
    const int alines    = m_ringAlineCount;
    QVector<float> raw(blockSize);
    QVector<float> ang(alines);
    QVector<quint8> ch(alines);
    std::memcpy(raw.data(), reinterpret_cast<float *>(h + 1),
                static_cast<size_t>(blockSize) * sizeof(float));
    std::memcpy(ang.data(),
                reinterpret_cast<uint8_t *>(h + 1) + ringAnglesOffset(blockSize),
                static_cast<size_t>(alines) * sizeof(float));
    std::memcpy(ch.data(),
                reinterpret_cast<uint8_t *>(h + 1) + ringChannelsOffset(blockSize, alines),
                static_cast<size_t>(alines));
    h->block_ready = 0;
    m_ringSharedMemory->unlock();

    const int sampDepth = m_ringConfig.sampDepth;
    const int perCh = m_ringConfig.alinesPerChannelPerBlock;   // 每通道块内触发数
    const int M = m_ringChannelCount;
    const double sectorWidthDeg = (M > 0) ? 360.0 / M : 0.0;   // 拼接模式扇区宽度
    const bool oddIsWl1 = (m_ringConfig.triggerWlOdd != 0);

    int selIdx[8];
    int si = 0;
    for (int c = 0; c < 8; ++c) selIdx[c] = -1;
    for (int c = 0; c < 8; ++c)
        if (m_ringChannels[c]) selIdx[c] = si++;

    // 每波长：所有选中通道的预处理后 A-line（各自独立长度 nt）
    std::vector<std::vector<float>> wlData[2];
    std::vector<float> wlAng[2];
    std::vector<float> wlRad[2];   // 每根 A-line 的重建半径（多扫描半径配准，m）
    std::vector<float> wlSecTh0[2]; // 每根 A-line 的扇区起点（度，拼接模式）

    for (int c = 0; c < 8; ++c) {
        if (!m_ringChannels[c]) continue;
        std::vector<std::vector<float>> lists[2];
        std::vector<float> angs[2];
        std::vector<float> rads[2];
        std::vector<float> secs[2];
        const double chSectorStartDeg =
            m_ringConfig.sectorStartDeg + static_cast<double>(selIdx[c]) * sectorWidthDeg;

        for (int j = 0; j < perCh; ++j) {
            const int pos = j * M + selIdx[c];
            const bool isWl1 = oddIsWl1 ? (j % 2 == 0) : (j % 2 == 1);
            const int w = isWl1 ? 0 : 1;

            std::vector<double> d(sampDepth);
            const float *src = raw.constData() + static_cast<size_t>(pos) * sampDepth;
            for (int r = 0; r < sampDepth; ++r) d[r] = static_cast<double>(src[r]);

            ringrecon::PreprocessParams pp;
            const int phCh = static_cast<int>(ch[pos]);   // 该 A-line 的物理通道号
            const int phIdx = (phCh >= 0 && phCh < 8) ? phCh : 0;
            pp.systemDelay = m_ringSysDelayCh[phIdx][w];
            pp.dbrmaskExtra = (w == 1 ? m_ringSysDelayCh[phIdx][1] - m_ringSysDelayCh[phIdx][0] : 0);
            pp.maskLength = m_ringConfig.maskLength;
            pp.dbrRemove = m_ringConfig.dbrSigRemove != 0;
            pp.delayCut = m_ringConfig.delayCut != 0;
            pp.signalImpair = m_ringConfig.singalImpair != 0;
            pp.imValue = m_ringConfig.imValue[w];

            lists[w].push_back(ringrecon::preprocessBlock(d, sampDepth, 1, pp));
            angs[w].push_back(ang[pos]);
            rads[w].push_back(static_cast<float>(
                m_ringConfig.radiusPerChannel[(phCh >= 0 && phCh < 8) ? phCh : 0]));
            secs[w].push_back(static_cast<float>(chSectorStartDeg));
        }

        // wl2 通道内跨块对齐：首块不偏移，后续块前插上一块末根
        if (m_ringConfig.shiftWL2 && m_ringBlockIndex > 0 && !m_ringPrevWL2[c].empty()) {
            lists[1].insert(lists[1].begin(), m_ringPrevWL2[c]);
            lists[1].pop_back();
            angs[1].insert(angs[1].begin(), m_ringPrevAngle[c]);
            angs[1].pop_back();
            rads[1].insert(rads[1].begin(), m_ringPrevRadius[c]);
            rads[1].pop_back();
            secs[1].insert(secs[1].begin(), static_cast<float>(chSectorStartDeg));
            secs[1].pop_back();
        }
        if (!lists[1].empty()) {
            m_ringPrevWL2[c] = lists[1].back();
            m_ringPrevAngle[c] = angs[1].back();
            m_ringPrevRadius[c] = rads[1].back();
        }

        for (int w = 0; w < 2; ++w) {
            for (size_t k = 0; k < lists[w].size(); ++k) {
                wlData[w].push_back(std::move(lists[w][k]));
                wlAng[w].push_back(angs[w][k]);
                wlRad[w].push_back(rads[w][k]);
                wlSecTh0[w].push_back(secs[w][k]);
            }
        }
    }

    // 展平并送入 CUDA（每根 A-line 独立角度 + 独立重建半径）
    std::vector<float> flat[2];
    int nd[2] = {0, 0};
    for (int w = 0; w < 2; ++w) {
        nd[w] = static_cast<int>(wlData[w].size());
        for (auto &v : wlData[w])
            flat[w].insert(flat[w].end(), v.begin(), v.end());
        if (nd[w] == 0 || flat[w].empty()) {
            sendError("波长数据为空", 2008);
            return;
        }
        const int nt = static_cast<int>(flat[w].size() / nd[w]);
        const int rc = m_ringConfig.spliceMode
            ? ring_recon_cuda_append_angles_radii_sector(
                  m_ringCuda[w], flat[w].data(), nt, nd[w],
                  wlAng[w].data(), wlRad[w].data(),
                  wlSecTh0[w].data(), static_cast<float>(sectorWidthDeg))
            : ring_recon_cuda_append_angles_radii(
                  m_ringCuda[w], flat[w].data(), nt, nd[w],
                  wlAng[w].data(), wlRad[w].data());
        if (rc != 0) {
            const QString fn = m_ringConfig.spliceMode
                ? QStringLiteral("ring_recon_cuda_append_angles_radii_sector")
                : QStringLiteral("ring_recon_cuda_append_angles_radii");
            sendError(QString("%1 failed: %2")
                          .arg(fn, QString::fromUtf8(ring_recon_cuda_last_error())), 2009);
            return;
        }
    }

    // 方案A：显示快照=全分辨率归一化帧（acc/accW），直接作为显示图像
    if (ring_recon_cuda_snapshot(m_ringCuda[0], m_ringDisplayNx, m_ringDisplayStep,
                                 m_ringDisplay0.data()) != 0 ||
        ring_recon_cuda_snapshot(m_ringCuda[1], m_ringDisplayNx, m_ringDisplayStep,
                                 m_ringDisplay1.data()) != 0) {
        sendError(QString("ring_recon_cuda_snapshot failed: %1")
                      .arg(QString::fromUtf8(ring_recon_cuda_last_error())), 2011);
        return;
    }
    ++m_ringBlockIndex;

    m_ringSharedMemory->lock();
    h = static_cast<RingImagingShmHeader *>(m_ringSharedMemory->data());
    float *fb = reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(h + 1) +
                                          ringFramesOffset(blockSize, alines));
    std::memcpy(fb, m_ringDisplay0.data(),
                static_cast<size_t>(m_ringFrameSize) * sizeof(float));
    std::memcpy(fb + m_ringFrameSize, m_ringDisplay1.data(),
                static_cast<size_t>(m_ringFrameSize) * sizeof(float));
    h->frame_seq++;
    m_ringSharedMemory->unlock();

    sendRingSnapshotToHost();     // 新链路：方案A 显示快照

    // 整圈完成：清零累积器，避免跨圈污染（PNG 保存由接收端窗口在圈末触发点执行）
    if (m_ringBlocksPerFrame > 0 && m_ringBlockIndex % m_ringBlocksPerFrame == 0) {
        resetRingRecon();
    }
}

void ImagingSvc::resetRingRecon()
{
    if (m_ringCuda[0]) ring_recon_cuda_reset(m_ringCuda[0]);
    if (m_ringCuda[1]) ring_recon_cuda_reset(m_ringCuda[1]);
    // 跨圈边界状态同步复位：使后续每圈与第 1 圈（首块无偏移、无上一圈末根）
    // 行为完全一致，消除第 2 圈起 wl2 末帧与第 1 圈不一致的问题。
    m_ringBlockIndex = 0;
    for (int c = 0; c < 8; ++c) {
        m_ringPrevWL2[c].clear();
        m_ringPrevAngle[c] = 0.0f;
        m_ringPrevRadius[c] = 0.0f;
    }
}

void ImagingSvc::sendRingSnapshotToHost()
{
    if (!m_ringSharedMemory) return;
    m_ringSharedMemory->lock();
    auto *h = static_cast<RingImagingShmHeader *>(m_ringSharedMemory->data());
    const int seq = static_cast<int>(h->frame_seq);
    m_ringSharedMemory->unlock();

    QJsonObject msg;
    msg["cmd"] = "ring_snapshot_ready";
    msg["seq"] = seq;
    QJsonDocument doc(msg);
    QByteArray data = doc.toJson(QJsonDocument::Compact);
    zmq::message_t zmsg(static_cast<size_t>(data.size()));
    std::memcpy(zmsg.data(), data.constData(), static_cast<size_t>(data.size()));
    try {
        if (m_zmqSocket) m_zmqSocket->send(zmsg, zmq::send_flags::dontwait);
    } catch (...) {
    }
}

// =====================================================================
// 脉冲处理：从共享内存读取 → 送入 GPU → 累积帧 → 输出
// =====================================================================
void ImagingSvc::processPulse()
{
    if (m_ringMode) return;   // 环形模式走 processRingPulse
    if (!m_running || !m_reconstructor || !m_sharedMemory) return;

    // 从共享内存读取脉冲数据
    m_sharedMemory->lock();
    auto *h = static_cast<ImagingShmHeader *>(m_sharedMemory->data());
    float *pulseBuf = reinterpret_cast<float *>(h + 1);
    int pulseSize = m_generalParams.physicalChannels * m_generalParams.depth;

    std::memcpy(m_pulseBuffer.data(), pulseBuf,
                static_cast<size_t>(pulseSize) * sizeof(float));
    h->pulse_ready = 0;
    m_sharedMemory->unlock();

    // 送入 GPU 重建
    int st = m_reconstructor->single_pulse_data(m_pulseBuffer);
    if (st != 0) {
        sendError(QString("pa_recon_single_pulse failed: %1").arg(st), 1002);
        return;
    }

    m_pulseCount++;

    // 每 50 脉冲输出一次进度
    if (m_pulseCount % 50 == 0) {
        qDebug() << "[ImagingSvc] pulseCount=" << m_pulseCount
                 << "/" << m_scanParams.move_aline
                 << "firstSample=" << m_pulseBuffer[0]
                 << "lastSample=" << m_pulseBuffer[m_pulseBuffer.size() - 1];
    }

    // 累积达到 move_aline 个脉冲 → 输出一帧
    if (m_pulseCount >= m_scanParams.move_aline) {
        qDebug() << "[ImagingSvc] Frame ready at pulseCount=" << m_pulseCount;
        sendFrameToHost();
        m_pulseCount = 0;
    }
}

// =====================================================================
// 将重建帧写回共享内存并通知主进程
// =====================================================================
void ImagingSvc::sendFrameToHost()
{
    if (!m_reconstructor || !m_sharedMemory) return;

    int frameSize = m_scanParams.nx * m_scanParams.ny;
    QVector<float> frameData(frameSize);

    if (m_reconstructor->get_output(frameData) != 0) {
        sendError("get_output failed", 1003);
        return;
    }

    // 检查输出是否为空或全零
    bool allZero = true;
    float maxVal = 0.0f, minVal = 0.0f;
    if (!frameData.isEmpty()) {
        minVal = maxVal = frameData[0];
        for (int i = 0; i < frameData.size(); ++i) {
            if (frameData[i] != 0.0f) { allZero = false; break; }
        }
        for (int i = 1; i < frameData.size(); ++i) {
            if (frameData[i] < minVal) minVal = frameData[i];
            if (frameData[i] > maxVal) maxVal = frameData[i];
        }
    }
    qDebug() << "[ImagingSvc] sendFrameToHost: frameSize=" << frameData.size()
             << "allZero=" << allZero
             << "range=[" << minVal << "," << maxVal << "]";

    // 写共享内存
    m_sharedMemory->lock();
    auto *h  = static_cast<ImagingShmHeader *>(m_sharedMemory->data());
    int ps = m_generalParams.physicalChannels * m_generalParams.depth;
    float *fb = reinterpret_cast<float *>(
        reinterpret_cast<uint8_t *>(h + 1) + static_cast<size_t>(ps) * sizeof(float));

    std::memcpy(fb, frameData.constData(),
                static_cast<size_t>(frameSize) * sizeof(float));
    h->frame_seq++;
    h->frame_ready = 1;
    m_sharedMemory->unlock();

    // 通知主进程
    QJsonObject msg;
    msg["cmd"] = "frame_ready";
    msg["seq"] = static_cast<int>(h->frame_seq);

    QJsonDocument doc(msg);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    zmq::message_t zmsg(static_cast<size_t>(data.size()));
    std::memcpy(zmsg.data(), data.constData(), static_cast<size_t>(data.size()));

    try {
        if (m_zmqSocket) {
            m_zmqSocket->send(zmsg, zmq::send_flags::dontwait);
        }
    } catch (...) {
        // 静默
    }

    sendStatus(2.0f);
}

// =====================================================================
// 状态/错误发送
// =====================================================================
void ImagingSvc::sendStatus(float fps)
{
    QJsonObject msg;
    msg["cmd"] = "status";
    msg["fps"] = static_cast<double>(fps);

    QJsonDocument doc(msg);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    zmq::message_t zmsg(static_cast<size_t>(data.size()));
    std::memcpy(zmsg.data(), data.constData(), static_cast<size_t>(data.size()));

    try {
        if (m_zmqSocket) {
            m_zmqSocket->send(zmsg, zmq::send_flags::dontwait);
        }
    } catch (...) {
        // 静默
    }
}

void ImagingSvc::sendError(const QString &errMsg, int code)
{
    QJsonObject msg;
    msg["cmd"] = "error";
    msg["msg"] = errMsg;
    msg["code"] = code;

    QJsonDocument doc(msg);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    zmq::message_t zmsg(static_cast<size_t>(data.size()));
    std::memcpy(zmsg.data(), data.constData(), static_cast<size_t>(data.size()));

    try {
        if (m_zmqSocket) {
            m_zmqSocket->send(zmsg, zmq::send_flags::dontwait);
        }
    } catch (...) {
        // 静默
    }
}
