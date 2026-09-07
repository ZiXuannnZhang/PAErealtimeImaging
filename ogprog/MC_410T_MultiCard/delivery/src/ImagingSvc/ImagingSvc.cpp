#include "ImagingSvc.h"
#include "ImagingSharedMemory.h"
#include "pa_recon_qt.hpp"
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
{
}

ImagingSvc::~ImagingSvc()
{
    m_running = false;
    delete m_reconstructor;

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

    // 附接共享内存
    m_sharedMemory = new QSharedMemory("MC_410T_ImagingShm", this);
    if (!m_sharedMemory->attach()) {
        qCritical() << "ImagingSvc: Failed to attach shared memory";
        return false;
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
    } else if (cmd == "start") {
        m_running = true;
        m_pulseCount = 0;
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
// 脉冲处理：从共享内存读取 → 送入 GPU → 累积帧 → 输出
// =====================================================================
void ImagingSvc::processPulse()
{
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
