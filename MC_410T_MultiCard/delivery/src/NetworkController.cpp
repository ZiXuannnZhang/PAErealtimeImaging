#include "NetworkController.h"
#include "FileSaver.h"
#include <QTimer>
#include <QDebug>
#include <QMetaObject>
#include <thread>
#include <chrono>
#include <algorithm>
#ifdef _WIN32
#include <iphlpapi.h>
#include <icmpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif


#ifdef _WIN32
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

static uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

NetworkController::NetworkController(QObject* parent)
    : QObject(parent)
    , m_controlSocket(INVALID_SOCKET)
    , m_feedbackSocket(INVALID_SOCKET) {}

NetworkController::~NetworkController() {
    // 析构时需要同步等待后台停止线程（不能留 this 指针悬空）
    if (m_running) {
        // 直接同步停止所有子线程（析构路径允许短暂阻塞）
        m_running = false;
        if (m_statsTimer) { m_statsTimer->stop(); delete m_statsTimer; m_statsTimer = nullptr; }
        for (auto& r : m_receivers) r->requestStop();
        for (auto& p : m_processors) p->requestStop();
        for (auto& s : m_savers) s->requestStop();
        if (m_publisher) m_publisher->requestInterruption();
        for (auto& r : m_receivers) {
            if (!r->wait(2000)) {
                r->forceCloseSockets();   // 先关闭 socket 再等待，避免 terminate 泄漏端口
                r->wait();   // 阻塞等待线程结束，避免析构运行中的 QThread
            }
        }
        for (auto& p : m_processors) { if (!p->wait(500)) { p->terminate(); p->wait(); } }
        for (auto& s : m_savers) { if (!s->wait(500)) { s->terminate(); s->wait(); } }
        if (m_publisher) { if (!m_publisher->wait(500)) { m_publisher->terminate(); m_publisher->wait(); } }
    }
    // 停止反馈监听线程
    m_feedbackRunning = false;
    if (m_feedbackThread.joinable()) {
        // 给 socket 发送一个空包或关闭 socket 让 recvfrom 返回
        cleanupFeedbackListener();
        m_feedbackThread.join();
    }

    // 等待后台停止线程完成（不能 detach，那样 this 会悬空）
    if (m_stopThread.joinable()) m_stopThread.join();
    cleanupControlSocket();
}

// ─────────────────────────────────────────────────────────────────────
// start()  根据 AcqConfig 创建全部线程
// ─────────────────────────────────────────────────────────────────────
bool NetworkController::start(const AcqConfig& config, std::function<void()> onStarted, std::function<void()> onFailed) {
    if (m_running) stop();
    m_config  = config;
    m_running = true;

    // 重建目标IP列表（优先使用网段扫描结果；否则默认 192.168.0.2 起每卡+1）
    m_targetIPs.clear();
    if (!config.targetIPs.empty()) {
        for (const auto& s : config.targetIPs)
            m_targetIPs.append(QString::fromStdString(s));
    } else {
        for (int i = 0; i < config.nCards; ++i)
            m_targetIPs.append(QString("192.168.0.%1").arg(i + 2));
    }

    // 初始化卡就绪状态
    m_cardsReady.assign(config.nCards, false);
    m_cmdQueue.clear();

    // 初始化配置确认状态
    m_configPhase   = ConfigPhase::Idle;
    m_configAck.clear();
    m_configRetry.clear();
    m_configSentMs.clear();

    // 初始化控制 socket（WSAStartup 已由此调用处理）
    initControlSocket();

    // 启动反馈监听线程（检测 18 字节就绪包）
    initFeedbackListener();

    const int nCards = config.nCards;

    //  可选扩展：FramePublisher 
    m_publisher = std::make_unique<FramePublisher>(this);
    m_publisher->configure(config.enablePublisher, nCards, config.samplesPerTrig());
    if (config.enablePublisher)
        m_publisher->start();

    // 每卡创建：DisplayBuffer + FileSaver + DataProcessor
    m_processors.clear();
    m_displayBuffers.clear();
    m_savers.clear();
    m_lastPktsReceived.assign(nCards, 0);
    m_lastPktsDropped.assign(nCards, 0);
    m_lastTrigsComplete.assign(nCards, 0);

    for (int i = 0; i < nCards; ++i) {
        auto dispBuf  = std::make_unique<DisplayBuffer>();
        auto saver    = std::make_unique<FileSaver>(i, this);
        // 自动保存会话代：保存器按触发组携带的会话代路由目录（gen=0 保持当前目录）
        saver->setSessionDirResolver([this](uint64_t gen) { return sessionDir(gen); });
        auto proc     = std::make_unique<DataProcessor>(
            i,
            saver->saveQueue(),
            dispBuf.get(),
            config.enablePublisher ? m_publisher.get() : nullptr,
            config,
            m_ringFeedSink
        );
        // 入队前打标：读取当前会话代（UI 线程在边界空闲期提前推进）
        proc->setSessionGenReader([this]() { return autoSessionGen(); });
        connect(saver.get(), &FileSaver::statusMessage,
                this, &NetworkController::statusMessage);
        connect(saver.get(), &FileSaver::errorOccurred,
                this, &NetworkController::errorOccurred);
        connect(proc.get(), &DataProcessor::partialTrigger,
                this, [this](int cardId, uint16_t seq, int missing) {
            emit statusMessage(QString("⚠ 卡%1 部分触发 seq=%2 缺%3包")
                               .arg(cardId + 1).arg(seq).arg(missing));
        });

        m_displayBuffers.push_back(std::move(dispBuf));
        m_savers.push_back(std::move(saver));
        m_processors.push_back(std::move(proc));
    }

    //  启动 DataProcessor 线程 
    for (auto& p : m_processors) p->start();
    //  启动 FileSaver 线程 
    for (auto& s : m_savers) s->start();

    //  创建并启动接收线程组 
    m_receivers.clear();
    std::vector<DataProcessor*> procPtrs;
    for (auto& p : m_processors) procPtrs.push_back(p.get());

    // ─────────────────────────────────────────────────────────────
    // 路线A：WinSock select 多路复用
    // 每 4 张卡一个 MultiPortReceiver，绑定独立 CPU 核
    // ─────────────────────────────────────────────────────────────
    const int CARDS_PER_RECEIVER = 4;
    int cpuCore = 2;  // 从 P 核 2 开始分配
    for (int g = 0; g < nCards; g += CARDS_PER_RECEIVER) {
        std::vector<int>             cardIdx;
        std::vector<DataProcessor*>  procs;
        for (int k = g; k < std::min(g + CARDS_PER_RECEIVER, nCards); ++k) {
            cardIdx.push_back(k);
            procs.push_back(procPtrs[k]);
        }
        auto recv = std::make_unique<MultiPortReceiver>(cardIdx, procs, cpuCore++, this);
        connect(recv.get(), &MultiPortReceiver::statusMessage,
                this, &NetworkController::statusMessage);
        connect(recv.get(), &MultiPortReceiver::errorOccurred,
                this, &NetworkController::errorOccurred);
        recv->start();
        m_receivers.push_back(std::move(recv));
    }

    // ══ 启动确认：所有接收线程必须完成端口绑定并进入接收循环 ══
    // 任一端口绑定失败（如端口被残留进程占用）时立即回滚，
    // 避免 UI 误报“监听已启动”而实际无任何数据接收。
    constexpr int RECEIVER_STARTUP_WAIT_MS = 3000;
    bool receiversReady = true;
    for (auto& r : m_receivers) {
        if (!r->waitUntilStarted(RECEIVER_STARTUP_WAIT_MS)) {
            receiversReady = false;
            break;
        }
    }
    if (!receiversReady) {
        rollbackStart();
        emit errorOccurred("接收端口启动失败，网络监听未启动（请检查 8001+ 端口是否被占用）");
        if (onFailed) onFailed();
        return false;
    }

    emit statusMessage(QString("NetworkController: 路线A（WinSock）启动，%1 卡，%2 接收组")
                       .arg(nCards).arg(m_receivers.size()));

    //  统计定时器 
    m_lastStatsMs = nowMs();
    m_statsTimer = new QTimer(this);
    connect(m_statsTimer, &QTimer::timeout, this, &NetworkController::onStatsTimer);
    m_statsTimer->start(STATS_UPDATE_MS);

    // 路线A同步完成，调用回调通知 UI
    if (onStarted) onStarted();
    return true;
}

// ═════════════════════════════════════════════════════════════════════
// rollbackStart()  接收线程启动失败时同步清理本次 start() 已创建的资源
// ═════════════════════════════════════════════════════════════════════
void NetworkController::rollbackStart()
{
    m_running = false;
    if (m_statsTimer) { m_statsTimer->stop(); delete m_statsTimer; m_statsTimer = nullptr; }
    m_feedbackRunning = false;
    cleanupFeedbackListener();
    if (m_feedbackThread.joinable()) m_feedbackThread.join();
    if (m_retryTimer) { m_retryTimer->stop(); delete m_retryTimer; m_retryTimer = nullptr; }
    if (m_probeTimer) { m_probeTimer->stop(); delete m_probeTimer; m_probeTimer = nullptr; }
    if (m_configTimer) { m_configTimer->stop(); delete m_configTimer; m_configTimer = nullptr; }
    for (auto& r : m_receivers) r->requestStop();
    for (auto& p : m_processors) p->requestStop();
    for (auto& s : m_savers) s->requestStop();
    if (m_publisher) m_publisher->requestInterruption();
    for (auto& r : m_receivers) {
        if (!r->wait(2000)) {
            r->forceCloseSockets();   // 先关闭 socket 再等待，避免 terminate 泄漏端口
            r->wait();   // 阻塞等待线程结束，避免析构运行中的 QThread
        }
    }
    for (auto& p : m_processors) { if (!p->wait(1000)) { p->terminate(); p->wait(); } }
    for (auto& s : m_savers) { if (!s->wait(1000)) { s->terminate(); s->wait(); } }
    if (m_publisher) { if (!m_publisher->wait(1000)) { m_publisher->terminate(); m_publisher->wait(); } }
    m_receivers.clear();
    m_processors.clear();
    m_savers.clear();
    m_displayBuffers.clear();
    m_publisher.reset();
    m_cmdQueue.clear();
    m_cardsReady.assign(m_config.nCards, false);
    m_configPhase = ConfigPhase::Idle;
    m_configAck.clear();
    m_configRetry.clear();
    m_configSentMs.clear();
    m_probeRetryCount = 0;
    cleanupControlSocket();
}

// ─────────────────────────────────────────────────────────────────────
// stop()  有序停止
// ─────────────────────────────────────────────────────────────────────
void NetworkController::stop() {
    // 防止重复调用
    if (!m_running && !m_stopThread.joinable()) {
        QMetaObject::invokeMethod(this, [this]() { emit stopped(); }, Qt::QueuedConnection);
        return;
    }
    if (!m_running && m_stopThread.joinable()) return;  // 后台停止线程已在运行

    m_running = false;

    if (m_statsTimer) {
        m_statsTimer->stop();
        m_statsTimer->deleteLater();
        m_statsTimer = nullptr;
    }

    // 停止反馈监听
    m_feedbackRunning = false;
    cleanupFeedbackListener();
    if (m_feedbackThread.joinable()) m_feedbackThread.join();

    // 清除命令重试 + 主动探测 + 配置确认
    if (m_retryTimer) { m_retryTimer->stop(); m_retryTimer->deleteLater(); m_retryTimer = nullptr; }
    if (m_probeTimer) { m_probeTimer->stop(); m_probeTimer->deleteLater(); m_probeTimer = nullptr; }
    if (m_configTimer) { m_configTimer->stop(); m_configTimer->deleteLater(); m_configTimer = nullptr; }
    m_probeRetryCount = 0;
    m_cmdQueue.clear();
    m_cardsReady.assign(m_config.nCards, false);
    m_configPhase = ConfigPhase::Idle;
    m_configAck.clear();
    m_configRetry.clear();
    m_configSentMs.clear();

    // 发出所有停止信号（非阻塞，立即返回）
    for (auto& r : m_receivers) r->requestStop();
    for (auto& p : m_processors) p->requestStop();
    for (auto& s : m_savers) s->requestStop();
    if (m_publisher) m_publisher->requestInterruption();

    // 把容器所有权移入后台线程，避免主线程析构时阻塞
    using RecvVec  = std::vector<std::unique_ptr<MultiPortReceiver>>;
    using ProcVec  = std::vector<std::unique_ptr<DataProcessor>>;
    using SaveVec  = std::vector<std::unique_ptr<FileSaver>>;
    using PubPtr   = std::unique_ptr<FramePublisher>;
    using DispVec  = std::vector<std::unique_ptr<DisplayBuffer>>;

    auto recvMoved = std::make_shared<RecvVec>(std::move(m_receivers));
    auto procMoved = std::make_shared<ProcVec>(std::move(m_processors));
    auto saveMoved = std::make_shared<SaveVec>(std::move(m_savers));
    auto pubMoved  = std::make_shared<PubPtr>(std::move(m_publisher));
    auto dispMoved = std::make_shared<DispVec>(std::move(m_displayBuffers));

    if (m_stopThread.joinable()) m_stopThread.detach();

    m_stopThread = std::thread([this, recvMoved, procMoved, saveMoved, pubMoved, dispMoved]() {
        constexpr int WAIT_MS = 500;
        constexpr int RECV_WAIT_MS = 2000;
        for (auto& r : *recvMoved) {
            if (!r->wait(RECV_WAIT_MS)) {
                r->forceCloseSockets();   // 先关闭 socket 再等待，避免 terminate 泄漏端口
                r->wait();   // 阻塞等待线程结束，避免析构运行中的 QThread
            }
        }
        recvMoved->clear();


        for (auto& p : *procMoved) { if (!p->wait(WAIT_MS)) { p->terminate(); p->wait(); } }
        procMoved->clear();
        for (auto& s : *saveMoved) { if (!s->wait(WAIT_MS)) { s->terminate(); s->wait(); } }
        saveMoved->clear();
        if (*pubMoved) { if (!(*pubMoved)->wait(WAIT_MS)) { (*pubMoved)->terminate(); (*pubMoved)->wait(); } pubMoved->reset(); }
        dispMoved->clear();
        QMetaObject::invokeMethod(this, [this]() {
            emit statusMessage("NetworkController: 已完全停止");
            emit stopped();
        }, Qt::QueuedConnection);
    });
}

// ─
// 存储控制
// ─
void NetworkController::startSaving(const QString& directory,
                                     int triggersPerFile,
                                     const QString& suffix) {
    for (auto& p : m_processors) p->setSaveEnabled(true);
    for (auto& s : m_savers)
        s->startSaving(directory, triggersPerFile, suffix);
}

void NetworkController::stopSaving() {
    for (auto& p : m_processors) p->setSaveEnabled(false);
    for (auto& s : m_savers) s->stopSaving();
}

// ─
// 自动保存会话代目录注册表（方案2 精确分界）
// ─
void NetworkController::registerSessionDir(uint64_t gen, const QString& dir) {
    QMutexLocker locker(&m_sessionDirMutex);
    m_sessionDirs.insert(gen, dir);
}

QString NetworkController::sessionDir(uint64_t gen) const {
    if (gen == 0) return QString();   // 手动模式：保持当前目录
    QMutexLocker locker(&m_sessionDirMutex);
    return m_sessionDirs.value(gen);
}

void NetworkController::requestCloseSavers() {
    for (auto& s : m_savers) s->requestClose();
}

void NetworkController::setDisplayPoints(int displayPoints) {
    if (displayPoints <= 0) return;
    m_config.displayPoints = displayPoints;
    for (auto& p : m_processors) p->setDisplayPoints(displayPoints);
}

void NetworkController::reconfigure(const AcqConfig& config) {
    m_config = config;
    for (auto& p : m_processors) p->updateConfig(config);
}

bool NetworkController::isSaving() const {
    return !m_savers.empty() && m_savers[0]->isSaving();
}

// ─
// 状态查询
// ─
std::optional<CardStats::Snapshot>
NetworkController::getCardStats(int cardIdx) const {
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_processors.size()))
        return std::nullopt;
    auto snap = m_processors[cardIdx]->statsSnapshot();
    if (cardIdx < static_cast<int>(m_savers.size()))
        snap.saveQueueDepth = m_savers[cardIdx]->queueDepth();
    return snap;
}

std::vector<CardStats::Snapshot> NetworkController::getAllCardStats() const {
    std::vector<CardStats::Snapshot> result;
    for (int i = 0; i < static_cast<int>(m_processors.size()); ++i) {
        auto snap = m_processors[i]->statsSnapshot();
        if (i < static_cast<int>(m_savers.size()))
            snap.saveQueueDepth = m_savers[i]->queueDepth();
        result.push_back(snap);
    }
    return result;
}

DisplayBuffer* NetworkController::displayBuffer(int cardIdx) const {
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_displayBuffers.size()))
        return nullptr;
    return m_displayBuffers[cardIdx].get();
}

// 
// 统计定时器（差分法计算速率，1Hz）
// 
void NetworkController::onStatsTimer() {
    updateAllStats();
}

void NetworkController::updateAllStats() {
    uint64_t now    = nowMs();
    double   dt     = static_cast<double>(now - m_lastStatsMs) / 1000.0;
    if (dt < 0.1) return;
    m_lastStatsMs = now;

    for (int i = 0; i < static_cast<int>(m_processors.size()); ++i) {
        auto& proc  = *m_processors[i];
        auto& stats = proc.stats();

        uint64_t curPkts = stats.packetsReceived.load(std::memory_order_relaxed);
        uint64_t curDrop = stats.packetsDropped.load(std::memory_order_relaxed);
        uint64_t curTrig = stats.triggersComplete.load(std::memory_order_relaxed);

        double dpkts = static_cast<double>(curPkts - m_lastPktsReceived[i]);
        double ddrop = static_cast<double>(curDrop - m_lastPktsDropped[i]);
        double dtrig = static_cast<double>(curTrig - m_lastTrigsComplete[i]);

        m_lastPktsReceived[i]  = curPkts;
        m_lastPktsDropped[i]   = curDrop;
        m_lastTrigsComplete[i] = curTrig;

        // Mbps = 包数  包大小(bit) / 时间(s) / 1e6
        stats.recvMbps       = dpkts * UDP_TOTAL_BYTES * 8.0 / dt / 1e6;
        stats.triggerHz      = dtrig / dt;
        // 丢包率（基于接收包和丢包的比例）
        double totalPkts = dpkts + ddrop;
        stats.packetLossRate = (totalPkts > 0) ? ddrop / totalPkts : 0.0;
        stats.lastUpdateMs   = m_lastStatsMs;
        // 队列深度（供 UI 监控积压情况）
        stats.inputQueueDepth = proc.inputQueueDepth();
    }
}

// =====================================================================
// UDP 控制命令实现（协议与 MC_410T_Qt 完全一致）
// =====================================================================

bool NetworkController::initControlSocket()
{
    cleanupControlSocket();
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    m_controlSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_controlSocket == INVALID_SOCKET) {
        emit errorOccurred("创建控制 socket 失败");
        return false;
    }

    // ── 绑定到指定本地IP，确保控制包从正确网络接口发出 ──────────────────
    // 双口网卡（如 ConnectX-5 MCX512A-ACAT）只接一个口时，必须绑定到
    // 已连接口的IP，否则 Windows 路由可能将数据包从另一个口（断路）发出，
    // 导致采集卡收不到控制命令，Wireshark 也抓不到任何控制包。
    // 配置方式：注册表 HKCU\Software\MC410T\MC410T_Receiver\NetworkParams\LocalBindIP
    if (!m_config.localBindIP.empty()) {
        sockaddr_in localAddr;
        memset(&localAddr, 0, sizeof(localAddr));
        localAddr.sin_family      = AF_INET;
        localAddr.sin_port        = 0;   // 本地端口任意
        inet_pton(AF_INET, m_config.localBindIP.c_str(), &localAddr.sin_addr);
        if (::bind(m_controlSocket,
                   reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) != 0) {
#ifdef _WIN32
            emit errorOccurred(
                QString("控制 socket 绑定本地IP %1 失败（WSA=%2），将使用默认路由，控制包可能从错误接口发出")
                .arg(QString::fromStdString(m_config.localBindIP)).arg(WSAGetLastError()));
#else
            emit errorOccurred(
                QString("控制 socket 绑定本地IP %1 失败，将使用默认路由")
                .arg(QString::fromStdString(m_config.localBindIP)));
#endif
        } else {
            emit statusMessage(
                QString("控制 socket 已绑定到本地接口 %1，控制包将从该接口发出")
                .arg(QString::fromStdString(m_config.localBindIP)));
        }
    } else {
        emit statusMessage("控制 socket 使用默认路由（提示：双口网卡建议在注册表 NetworkParams/LocalBindIP 中指定本地IP）");
    }
    return true;
}

void NetworkController::cleanupControlSocket()
{
    if (m_controlSocket != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(m_controlSocket);
        WSACleanup();
#else
        close(m_controlSocket);
#endif
        m_controlSocket = INVALID_SOCKET;
    }
}

// ═════════════════════════════════════════════════════════════════════
// 反馈监听（端口 8000）：检测采集卡发送的 18 字节就绪包
//
// 采集卡上电后 FPGA 网络栈初始化需要 ~130 秒，完成后每张卡会向
// 主机 IP:8000 发送一个 18 字节的 UDP 包作为"就绪"信号。
// 只有收到此包后，该卡才能正常接收配置/测量命令。
// ═════════════════════════════════════════════════════════════════════

bool NetworkController::initFeedbackListener()
{
    cleanupFeedbackListener();

    m_feedbackSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_feedbackSocket == INVALID_SOCKET) {
        emit errorOccurred("创建反馈监听 socket 失败");
        return false;
    }

    // 设置接收超时（1秒），使线程能响应退出请求
#ifdef _WIN32
    DWORD timeout = 1000;
    setsockopt(m_feedbackSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(m_feedbackSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    // 绑定到 FEEDBACK_PORT（8000），监听所有接口
    sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_port   = htons(FEEDBACK_PORT);
    localAddr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(m_feedbackSocket,
               reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) != 0) {
#ifdef _WIN32
        emit errorOccurred(QString("反馈 socket 绑定端口 %1 失败（WSA=%2），将无法检测卡片就绪状态")
                          .arg(FEEDBACK_PORT).arg(WSAGetLastError()));
#else
        emit errorOccurred(QString("反馈 socket 绑定端口 %1 失败，将无法检测卡片就绪状态")
                          .arg(FEEDBACK_PORT));
#endif
        cleanupFeedbackListener();
        return false;
    }

    m_feedbackRunning = true;
    m_feedbackThread = std::thread(&NetworkController::feedbackListenerThread, this);

    // 创建重试定时器（500ms 间隔，用于重新发送等待就绪的命令）
    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setInterval(500);
        connect(m_retryTimer, &QTimer::timeout, this, &NetworkController::retryPendingCommand);
    }

    // 启动主动探测定时器（8 秒后对未就绪卡进行 ARP 探测，
    // 解决上位机重启后漏掉 18 字节就绪包的问题）
    startProbeTimer();

    emit statusMessage(QString("反馈监听已启动（端口 %1），等待卡片就绪信号...").arg(FEEDBACK_PORT));
    return true;
}

void NetworkController::cleanupFeedbackListener()
{
    if (m_feedbackSocket != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(m_feedbackSocket);
#else
        close(m_feedbackSocket);
#endif
        m_feedbackSocket = INVALID_SOCKET;
    }
}

void NetworkController::feedbackListenerThread()
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    char buf[64];
    sockaddr_in senderAddr;
    socklen_t senderLen = sizeof(senderAddr);

    emit statusMessage("[反馈监听] 线程已启动");

    while (m_feedbackRunning) {
        int n = recvfrom(m_feedbackSocket, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);
        if (n <= 0) {
            // 超时或错误，继续循环检查 m_feedbackRunning
            continue;
        }

        // 将发送方 IP 映射到卡索引（18 字节就绪包 / 60 字节配置反馈共用）
        char srcIP[64];
        inet_ntop(AF_INET, &senderAddr.sin_addr, srcIP, sizeof(srcIP));
        QString ipStr(srcIP);

        int cardIdx = -1;
        for (int i = 0; i < m_targetIPs.size(); ++i) {
            if (m_targetIPs[i] == ipStr) {
                cardIdx = i;
                break;
            }
        }

        if (n == 18) {
            // ── 18 字节：卡版本号/就绪包 ─────────────────────────
            // 上电后主动上报；也可能在收到配置指令后立即回发。
            // 注意：这不代表配置成功，WaitingAck 时需重发配置。
            if (cardIdx < 0) {
                emit statusMessage(QString("[反馈监听] 收到未知来源的 18 字节包: %1（非目标卡IP）").arg(ipStr));
                continue;
            }
            QMetaObject::invokeMethod(this, [this, cardIdx]() {
                onReadyPacket(cardIdx);
            }, Qt::QueuedConnection);
        } else if (n == 60) {
            // ── 60 字节：配置参数反馈 ─────────────────────────────
            // 仅配置参数指令有此反馈，表示采集卡已收到配置。
            // 反馈按单卡独立上报（以来源 IP 区分）。
            if (cardIdx < 0) {
                emit statusMessage(QString("[反馈监听] 收到未知来源的 60 字节反馈: %1（非目标卡IP）").arg(ipStr));
                continue;
            }
            QMetaObject::invokeMethod(this, [this, cardIdx]() {
                onConfigAck(cardIdx);
            }, Qt::QueuedConnection);
        } else {
            emit statusMessage(QString("[反馈监听] 忽略未知包: %1 字节来自 %2")
                              .arg(n)
                              .arg(inet_ntoa(senderAddr.sin_addr)));
            continue;
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif
    emit statusMessage("[反馈监听] 线程已退出");
}

void NetworkController::markCardReady(int cardIdx, bool viaReadyPacket)
{
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_cardsReady.size()))
        return;
    if (m_cardsReady[cardIdx])
        return;  // 已标记，忽略重复

    m_cardsReady[cardIdx] = true;
    if (viaReadyPacket)
        emit statusMessage(QString("卡%1（%2）FPGA 就绪 ✓（收到 18 字节就绪包）")
                          .arg(cardIdx + 1).arg(m_targetIPs[cardIdx]));
    else
        emit statusMessage(QString("卡%1（%2）标记为就绪 ✓（ARP 探测）")
                          .arg(cardIdx + 1).arg(m_targetIPs[cardIdx]));
    emit cardReady(cardIdx);

    // 检查是否所有卡均已就绪
    if (isAllCardsReady()) {
        emit statusMessage("所有采集卡均已就绪");
        emit allCardsReady();

        // 如果有等待的命令，立即执行（FPGA 现在能正确处理了）
        if (!m_cmdQueue.empty()) {
            emit statusMessage("FPGA 就绪，开始执行等待中的命令...");
            retryPendingCommand();
        }
    } else {
        // 部分就绪时，如果有等待命令且刚就绪的卡是目标卡之一，也尝试重试
        if (!m_cmdQueue.empty()) {
            retryPendingCommand();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════
// 配置确认状态机（60 字节反馈）
//
// 背景：配置参数指令下发后，必须收到采集卡反馈的 60 字节 UDP 包才能
//       进行测量控制；否则采集卡内部会禁止执行开始测量指令。
//       反馈按单卡独立返回，重发仅针对未确认的卡。
// 特殊：采集卡上电后立即下发配置，会先收到 18 字节版本号包，
//       这不代表配置成功，必须重发配置直到收到 60 字节反馈。
// ═════════════════════════════════════════════════════════════════════

void NetworkController::onReadyPacket(int cardIdx)
{
    markCardReady(cardIdx, true);   // 真正收到 18 字节版本号/就绪包

    // 正在等待配置确认时收到 18 字节包：非配置成功反馈，需重发该卡配置
    if (m_configPhase == ConfigPhase::WaitingAck &&
        cardIdx >= 0 && cardIdx < static_cast<int>(m_configAck.size()) &&
        !m_configAck[cardIdx]) {
        emit statusMessage(QString("卡%1 收到 18 字节版本号包，重发配置参数...").arg(cardIdx + 1));
        m_configRetry[cardIdx] = 0;   // 卡处于活跃响应，重置重试计数
        doSendConfigTo(cardIdx);
    }
}

void NetworkController::beginConfigWait(const PendingConfig& pc)
{
    m_pendingConfig = pc;
    m_configPhase   = ConfigPhase::WaitingAck;

    const int n = static_cast<int>(m_targetIPs.size());
    m_configAck.assign(n, false);
    m_configRetry.assign(n, 0);
    m_configSentMs.assign(n, 0);

    if (!m_configTimer) {
        m_configTimer = new QTimer(this);
        m_configTimer->setInterval(CONFIG_ACK_TICK_MS);
        connect(m_configTimer, &QTimer::timeout, this, &NetworkController::onConfigTimerTick);
    }

    // 逐卡下发配置（per-card 独立确认）
    for (int i = 0; i < n; ++i)
        doSendConfigTo(i);

    m_configTimer->start();
    emit statusMessage(QString("配置参数已下发（%1 张卡），等待 60 字节反馈确认...").arg(n));
}

bool NetworkController::doSendConfigTo(int cardIdx)
{
    if (m_controlSocket == INVALID_SOCKET) return false;
    if (cardIdx < 0 || cardIdx >= m_targetIPs.size()) return false;
    QByteArray cmd = buildConfigPacket(m_pendingConfig.dataTime,
                                       m_pendingConfig.aDelay,
                                       m_pendingConfig.bDelay);
    bool ok = sendRawCommand(cmd, m_targetIPs[cardIdx]);
    if (cardIdx >= 0 && cardIdx < static_cast<int>(m_configSentMs.size()))
        m_configSentMs[cardIdx] = nowMs();
    return ok;
}

void NetworkController::onConfigAck(int cardIdx)
{
    if (m_configPhase != ConfigPhase::WaitingAck) return;
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_configAck.size())) return;
    if (m_configAck[cardIdx]) return;  // 已确认，忽略重复

    m_configAck[cardIdx] = true;
    emit statusMessage(QString("卡%1 配置确认 ✓（收到 60 字节反馈）").arg(cardIdx + 1));
    emit configAcked(cardIdx);

    if (isAllConfigAcked()) {
        m_configPhase = ConfigPhase::Confirmed;
        if (m_configTimer) m_configTimer->stop();
        emit statusMessage("所有采集卡配置参数均已确认");
        emit configConfirmed();
        // 若有等待配置确认的测量命令 → 自动执行
        retryPendingCommand();
    }
}

void NetworkController::onConfigTimerTick()
{
    if (m_configPhase != ConfigPhase::WaitingAck) return;

    const uint64_t now = nowMs();
    bool anyFailed = false;
    for (int i = 0; i < static_cast<int>(m_configAck.size()); ++i) {
        if (m_configAck[i]) continue;   // 已确认
        if (i >= static_cast<int>(m_configSentMs.size())) continue;
        if (now - m_configSentMs[i] < CONFIG_ACK_TIMEOUT_MS) continue;  // 未到超时

        if (m_configRetry[i] >= CONFIG_ACK_MAX_RETRY) {
            // 重发超限：判定失败（多卡联动，任一失败即整体失败）
            anyFailed = true;
            emit statusMessage(QString("卡%1 配置确认失败（多次重发未收到 60 字节反馈）").arg(i + 1));
            emit configAckFailed(i);
        } else {
            ++m_configRetry[i];
            emit statusMessage(QString("卡%1 配置反馈超时，重发配置（第 %2/%3 次）")
                              .arg(i + 1).arg(m_configRetry[i]).arg(CONFIG_ACK_MAX_RETRY));
            doSendConfigTo(i);
        }
    }

    if (anyFailed) {
        m_configPhase = ConfigPhase::Failed;
        if (m_configTimer) m_configTimer->stop();
        emit statusMessage("配置确认失败：存在未反馈的采集卡，请检查链路后重新下发配置");
    }
}

bool NetworkController::isAllConfigAcked() const
{
    return !m_configAck.empty() &&
           std::all_of(m_configAck.begin(), m_configAck.end(), [](bool v) { return v; });
}

void NetworkController::retryPendingCommand()
{
    if (m_cmdQueue.empty()) {
        if (m_retryTimer) m_retryTimer->stop();
        return;
    }

    // 未全部就绪：继续等待（重试定时器已在运行）
    if (!isAllCardsReady()) {
        if (m_retryTimer && !m_retryTimer->isActive())
            m_retryTimer->start();
        return;
    }

    // 处理队首命令（按入队顺序：先配置、后测量）
    const PendingCmd front = m_cmdQueue.front();
    if (front.type == PendingCmdType::Config) {
        // 配置已在下发确认中，等待其完成（避免重复下发）
        if (m_configPhase == ConfigPhase::WaitingAck) {
            if (m_retryTimer && !m_retryTimer->isActive()) m_retryTimer->start();
            return;
        }
        if (m_retryTimer) m_retryTimer->stop();
        m_cmdQueue.pop_front();
        beginConfigWait({front.dataTime, front.aDelay, front.bDelay});
        return;
    }

    // 测量命令（Start / Stop）：必须在配置确认后才能执行
    if (isConfigConfirmed()) {
        if (m_retryTimer) m_retryTimer->stop();
        bool result = (front.type == PendingCmdType::StartMeasure)
                    ? doSendStartMeasure() : doSendStopMeasure();
        if (result) emit statusMessage("待执行的命令已成功发送");
        else emit errorOccurred("待执行的命令发送失败");
        m_cmdQueue.pop_front();
        retryPendingCommand();   // 继续处理队列中的后续命令
        return;
    }
    if (m_configPhase == ConfigPhase::Failed) {
        if (m_retryTimer) m_retryTimer->stop();
        emit errorOccurred("配置未确认（存在失败卡），无法执行测量命令，请重新下发配置");
        m_cmdQueue.pop_front();
        return;
    }
    // Idle / WaitingAck：等待配置确认完成（configConfirmed 会再次触发本函数）
    if (m_retryTimer && !m_retryTimer->isActive()) m_retryTimer->start();
}

// ═════════════════════════════════════════════════════════════════════
// 主动探测（ARP 兜底）
//
// 场景 A：采集卡上电已久、已发送过 18 字节就绪包，但上位机 PC 刚刚重启，
//         此时被动监听永远等不到就绪包。
// 场景 B：冷启动时 ARP 先于 18 字节包就绪，提前标记。
//
// 机制：启动 8 秒后，对仍未就绪的卡调用 SendARP 探测其可达性。
//       若 ARP 能解析到 MAC 地址 → 卡已在线 → 标记为就绪。
//       注意：18 字节就绪包是卡冷启动时一次性发送的，后续重启上位机时
//       卡不会再发此包，因此 ARP 探测成功后必须直接标记就绪。
//       每轮探测间隔 5 秒，最多重试 6 轮（共约 38 秒）。
// ═════════════════════════════════════════════════════════════════════

void NetworkController::startProbeTimer()
{
    if (m_probeTimer) {
        m_probeTimer->stop();
        m_probeTimer->deleteLater();
    }
    m_probeRetryCount = 0;
    m_probeTimer = new QTimer(this);
    m_probeTimer->setSingleShot(true);
    connect(m_probeTimer, &QTimer::timeout, this, &NetworkController::onProbeTimeout);
    m_probeTimer->start(8000);  // 8 秒后首次探测
}

void NetworkController::onProbeTimeout()
{
    if (isAllCardsReady()) {
        // 全部已就绪，不需要探测
        return;
    }

    const int maxRetries = 6;
    if (++m_probeRetryCount > maxRetries) {
        emit statusMessage(QString("主动探测结束：仍有 %1/%2 张卡未就绪，可尝试重启采集卡")
                          .arg(m_targetIPs.size() - readyCardCount())
                          .arg(m_targetIPs.size()));
        return;
    }

    bool anyProbed = false;
    for (int i = 0; i < static_cast<int>(m_targetIPs.size()); ++i) {
        if (m_cardsReady[i]) continue;  // 已就绪，跳过

        // ── 用 SendARP 探测卡是否可达 ──────────────────────────
        QString ipStr = m_targetIPs[i];
        bool reachable = false;

#ifdef _WIN32
        ULONG macBuf[2] = {};      // 6 字节 MAC 用 2 个 ULONG 装
        ULONG macLen = sizeof(macBuf);
        IPAddr dest = inet_addr(ipStr.toStdString().c_str());

        DWORD arpRet = SendARP(dest, 0, macBuf, &macLen);
        if (arpRet == NO_ERROR && macLen >= 6) {
            reachable = true;
            emit statusMessage(QString("  ARP 探测成功: %1 → %02X:%02X:%02X:%02X:%02X:%02X")
                              .arg(ipStr)
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[0])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[1])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[2])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[3])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[4])
                              .arg(static_cast<uint8_t*>(static_cast<void*>(macBuf))[5]));
        }
#endif

        if (reachable) {
            // ARP 可达 → 卡已在线（重启上位机时卡不会重发18字节包）
            // 直接标记为就绪
            emit statusMessage(QString("卡%1（%2）ARP 探测成功，标记为就绪 ✓")
                              .arg(i + 1).arg(ipStr));
            markCardReady(i, false);   // ARP 探测就绪（并未收到 18 字节包）
            anyProbed = true;
        } else {
            emit statusMessage(QString("  卡%1（%2）ARP 探测不可达，%3 秒后重试...")
                              .arg(i + 1).arg(ipStr)
                              .arg(m_probeRetryCount < maxRetries ? "5" : "停止"));
        }
    }

    // 还有卡未就绪 → 5 秒后再次探测
    if (!isAllCardsReady()) {
        m_probeTimer->setSingleShot(true);
        m_probeTimer->start(5000);
    }
}

// ══ 卡就绪状态查询 ══════════════════════════════════════════════════
int NetworkController::readyCardCount() const
{
    return static_cast<int>(std::count(m_cardsReady.begin(), m_cardsReady.end(), true));
}

bool NetworkController::isCardReady(int cardIdx) const
{
    if (cardIdx < 0 || cardIdx >= static_cast<int>(m_cardsReady.size()))
        return false;
    return m_cardsReady[cardIdx];
}

bool NetworkController::isAllCardsReady() const
{
    return !m_cardsReady.empty() &&
           std::all_of(m_cardsReady.begin(), m_cardsReady.end(), [](bool v) { return v; });
}

// ══ 底层发送（不做就绪检查，由重试机制调用）════════════════════════════
static bool sendRawToAll(SocketType sock, const QByteArray& cmd,
                         const QVector<QString>& targets,
                         int* outSuccess, int* outFail)
{
    if (sock == INVALID_SOCKET) return false;
    int ok = 0, fail = 0;
    for (const QString& ip : targets) {
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(CONTROL_PORT));
        inet_pton(AF_INET, ip.toStdString().c_str(), &addr.sin_addr);
        int sent = sendto(sock, cmd.constData(), cmd.size(), 0,
                          reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (sent > 0) ++ok; else ++fail;
    }
    if (outSuccess) *outSuccess = ok;
    if (outFail)    *outFail    = fail;
    return ok > 0;
}

bool NetworkController::doSendConfigCommand(int dataTime, int aDelay, int bDelay)
{
    QByteArray cmd = buildConfigPacket(dataTime, aDelay, bDelay);
    int successCount = 0, failCount = 0;
    sendRawToAll(m_controlSocket, cmd, m_targetIPs, &successCount, &failCount);
    if (successCount > 0) {
        emit statusMessage(QString("配置命令已发送（%1/%2 张卡成功）: 采集=%3ns, A延时=%4ns, B延时=%5ns")
                          .arg(successCount).arg(m_targetIPs.size())
                          .arg(dataTime).arg(aDelay).arg(bDelay));
        if (failCount > 0)
            emit statusMessage(QString("另 %1 张发送失败，可能为尚未连接的卡").arg(failCount));
        return true;
    }
    return false;
}

bool NetworkController::doSendStartMeasure()
{
    const unsigned char startBytes[58] = {
        0xFA, 0xFA, 0xFA, 0xFA, 0x03, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01
    };
    QByteArray cmd(reinterpret_cast<const char*>(startBytes), sizeof(startBytes));
    int successCount = 0, failCount = 0;
    sendRawToAll(m_controlSocket, cmd, m_targetIPs, &successCount, &failCount);
    if (successCount > 0) {
        emit statusMessage(QString("开始测量命令已发送（%1/%2 张卡成功）")
                          .arg(successCount).arg(m_targetIPs.size()));
        if (failCount > 0)
            emit statusMessage(QString("另 %1 张未连接或发送失败").arg(failCount));
        return true;
    }
    return false;
}

bool NetworkController::doSendStopMeasure()
{
    // 停止测量 = 命令0x03 + 末尾0x00（与开始测量仅末尾字节不同）
    const unsigned char stopBytes[58] = {
        0xFA, 0xFA, 0xFA, 0xFA, 0x03, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    QByteArray cmd(reinterpret_cast<const char*>(stopBytes), sizeof(stopBytes));
    int successCount = 0, failCount = 0;
    sendRawToAll(m_controlSocket, cmd, m_targetIPs, &successCount, &failCount);
    if (successCount > 0) {
        emit statusMessage(QString("停止测量命令已发送（%1/%2 张卡成功）")
                          .arg(successCount).arg(m_targetIPs.size()));
        if (failCount > 0)
            emit statusMessage(QString("另 %1 张发送失败").arg(failCount));
        return true;
    }
    return false;
}

bool NetworkController::sendRawCommand(const QByteArray& cmd, const QString& targetIP)
{
    if (m_controlSocket == INVALID_SOCKET) return false;
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(CONTROL_PORT));
    inet_pton(AF_INET, targetIP.toStdString().c_str(), &addr.sin_addr);
    int sent = sendto(m_controlSocket, cmd.constData(), cmd.size(), 0,
                      reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    // 注意：不对每个 IP 单独 emit errorOccurred，避免 nCards 较大时日志刷屏。
    // 调用方（sendConfigCommand 等）统计成功/失败数后汇总上报。
    return (sent > 0);
}

// 构造配置包（58字节，与旧项目 buildConfigCommand 完全一致）
QByteArray NetworkController::buildConfigPacket(int dataTime, int aDelay, int bDelay)
{
    constexpr double FPGA_REG_NS = 4.0;
    int Trig        = static_cast<int>(bDelay   / FPGA_REG_NS);
    int ADC_capture = static_cast<int>(dataTime / FPGA_REG_NS);
    int adc_delay   = static_cast<int>(aDelay   / FPGA_REG_NS);

    QByteArray cmd;
    cmd.reserve(58);
    cmd.append('\xFA'); cmd.append('\xFA'); cmd.append('\xFA'); cmd.append('\xFA');
    cmd.append('\x02');
    cmd.append(static_cast<char>((Trig >> 24) & 0xFF));
    cmd.append(static_cast<char>((Trig >> 16) & 0xFF));
    cmd.append(static_cast<char>((Trig >>  8) & 0xFF));
    cmd.append(static_cast<char>( Trig        & 0xFF));
    cmd.append(static_cast<char>((ADC_capture >> 16) & 0xFF));
    cmd.append(static_cast<char>((ADC_capture >>  8) & 0xFF));
    cmd.append(static_cast<char>( ADC_capture        & 0xFF));
    cmd.append(static_cast<char>((adc_delay >> 16) & 0xFF));
    cmd.append(static_cast<char>((adc_delay >>  8) & 0xFF));
    cmd.append(static_cast<char>( adc_delay        & 0xFF));
    cmd.append(43, '\x00');
    return cmd;
}

bool NetworkController::sendConfigCommand(int dataTime, int aDelay, int bDelay)
{
    if (m_controlSocket == INVALID_SOCKET) {
        emit errorOccurred("控制 socket 未初始化，请先点击[开始监听]");
        return false;
    }

    if (isAllCardsReady()) {
        // ── 所有卡已就绪：逐卡下发配置并等待 60 字节反馈确认 ──
        beginConfigWait({dataTime, aDelay, bDelay});
        return true;
    }

    // ── 卡片未全部就绪：入队等待（队列保证配置不会被测量命令覆盖）──
    m_cmdQueue.push_back(PendingCmd{PendingCmdType::Config, dataTime, aDelay, bDelay});

    int readyCnt = readyCardCount();
    emit statusMessage(QString("配置命令已加入等待队列（%1/%2 张卡就绪，等待全部就绪后自动发送...")
                      .arg(readyCnt).arg(m_targetIPs.size()));

    // 启动重试定时器（如果还没启动）
    if (m_retryTimer && !m_retryTimer->isActive())
        m_retryTimer->start();

    return true;  // 返回 true 表示命令已接受
}

bool NetworkController::sendStartMeasure()
{
    if (m_controlSocket == INVALID_SOCKET) {
        emit errorOccurred("控制 socket 未初始化，请先点击[开始监听]");
        return false;
    }

    if (isAllCardsReady()) {
        if (isConfigConfirmed()) {
            // ── 配置已确认：直接发送（测量命令无反馈，不等待）──
            return doSendStartMeasure();
        }
        if (m_configPhase == ConfigPhase::Failed) {
            emit errorOccurred("配置未确认（存在失败卡），无法开始测量，请重新下发配置");
            return false;
        }
        // 等待配置确认完成后自动发送（configConfirmed 会触发 retryPendingCommand）
        m_cmdQueue.push_back(PendingCmd{PendingCmdType::StartMeasure, 0, 0, 0});
        emit statusMessage("等待配置参数确认完成后自动开始测量...");
        if (m_retryTimer && !m_retryTimer->isActive())
            m_retryTimer->start();
        return true;
    }

    // ── 卡片未全部就绪：入队等待（队列保证顺序）──
    m_cmdQueue.push_back(PendingCmd{PendingCmdType::StartMeasure, 0, 0, 0});

    int readyCnt = readyCardCount();
    emit statusMessage(QString("开始测量命令已加入等待队列（%1/%2 张卡就绪，等待全部就绪后自动发送...")
                      .arg(readyCnt).arg(m_targetIPs.size()));

    if (m_retryTimer && !m_retryTimer->isActive())
        m_retryTimer->start();

    return true;
}

bool NetworkController::sendStopMeasure()
{
    if (m_controlSocket == INVALID_SOCKET) {
        emit errorOccurred("控制 socket 未初始化，请先点击[开始监听]");
        return false;
    }

    if (isAllCardsReady()) {
        if (isConfigConfirmed()) {
            // ── 配置已确认：直接发送（测量命令无反馈，不等待）──
            return doSendStopMeasure();
        }
        if (m_configPhase == ConfigPhase::Failed) {
            emit errorOccurred("配置未确认（存在失败卡），无法停止测量");
            return false;
        }
        // 等待配置确认完成后自动发送（configConfirmed 会触发 retryPendingCommand）
        m_cmdQueue.push_back(PendingCmd{PendingCmdType::StopMeasure, 0, 0, 0});
        emit statusMessage("等待配置参数确认完成后自动停止测量...");
        if (m_retryTimer && !m_retryTimer->isActive())
            m_retryTimer->start();
        return true;
    }

    m_cmdQueue.push_back(PendingCmd{PendingCmdType::StopMeasure, 0, 0, 0});
    int readyCnt = readyCardCount();
    emit statusMessage(QString("停止测量命令已加入等待队列（%1/%2 张卡就绪，等待全部就绪后自动发送...")
                      .arg(readyCnt).arg(m_targetIPs.size()));
    if (m_retryTimer && !m_retryTimer->isActive())
        m_retryTimer->start();
    return true;
}

void NetworkController::setMeasureEnabled(bool enable)
{
    for (auto &p : m_processors) {
        if (p) p->setMeasureEnabled(enable);
    }
}

// ═════════════════════════════════════════════════════════════════════
// 网段扫描自动识别
//
// 专用网段假设：192.168.0.x 范围内可达的 IP 即采集卡（上位机与交换机
// 的 IP 会避开扫描范围）。扫描用于自动确定采集卡数量与目标 IP，
// 减少用户手动配置。扫描范围由注册表
//   NetworkParams/ScanBaseIP（默认 192.168.0.2）
//   NetworkParams/ScanIPCount（默认 32）
// 控制。
// ═════════════════════════════════════════════════════════════════════
#ifdef _WIN32
// ICMP 快扫（采集卡响应 ping）：短超时，比 SendARP 快一个数量级
static bool icmpProbeOnce(HANDLE hIcmp, const QString& ip, DWORD timeoutMs, bool* timedOut)
{
    IPAddr dest = inet_addr(ip.toStdString().c_str());
    char sendData[32] = {0};
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + 64;
    std::vector<char> replyBuf(replySize, 0);
    DWORD n = IcmpSendEcho(hIcmp, dest, sendData, sizeof(sendData), nullptr,
                           replyBuf.data(), replySize, timeoutMs);
    if (n > 0) { *timedOut = false; return true; }
    DWORD err = GetLastError();
    *timedOut = (err == IP_REQ_TIMED_OUT);   // 超时=离线，不视为探测错误
    return false;
}
// ARP 兜底（ICMP 未命中时，兼容不响应 ping 或 ICMP 被过滤的情况）
static bool arpProbeOnce(const QString& ip)
{
    ULONG macBuf[2] = {};
    ULONG macLen = sizeof(macBuf);
    IPAddr dest = inet_addr(ip.toStdString().c_str());
    DWORD arpRet = SendARP(dest, 0, macBuf, &macLen);
    return arpRet == NO_ERROR && macLen >= 6;
}
#endif

QVector<QString> NetworkController::scanReachableIPs(const QString& baseIP, int count)
{
    QVector<QString> result;
    const QStringList parts = baseIP.split('.');
    if (parts.size() != 4) return result;

    bool ok = false;
    const int baseOctet = parts[3].toInt(&ok);
    if (!ok) return result;
    const QString prefix = parts[0] + "." + parts[1] + "." + parts[2] + ".";

    // 生成候选 IP 列表
    std::vector<QString> ips;
    for (int i = 0; i < count; ++i) {
        const int octet = baseOctet + i;
        if (octet < 1 || octet > 254) break;
        ips.push_back(prefix + QString::number(octet));
    }
    const int total = static_cast<int>(ips.size());
    if (total <= 0) return result;

    // 并行探测：最多 16 线程分片，每线程只写自己的槽位（无竞争）。
    // ICMP 快扫（150ms 超时）为主判据：在线毫秒级响应，超时=离线直接判离线，
    // 只有 ICMP 调用出错（非超时）才用 SendARP 兜底，避免离线 IP 叠加 ARP 超时。
    std::vector<char> online(total, 0);
    const int nThreads = std::min<int>(16, total);
    std::vector<std::thread> workers;
    workers.reserve(nThreads);
    for (int t = 0; t < nThreads; ++t) {
        workers.emplace_back([&ips, &online, t, nThreads]() {
#ifdef _WIN32
            HANDLE hIcmp = IcmpCreateFile();
            if (hIcmp == INVALID_HANDLE_VALUE) hIcmp = nullptr;
            for (int i = t; i < static_cast<int>(ips.size()); i += nThreads) {
                bool timedOut = false;
                bool reachable = (hIcmp && icmpProbeOnce(hIcmp, ips[i], SCAN_ICMP_TIMEOUT_MS, &timedOut));
                if (!reachable && !timedOut) reachable = arpProbeOnce(ips[i]);   // 仅出错时 ARP 兜底
                online[i] = reachable ? 1 : 0;
            }
            if (hIcmp) IcmpCloseHandle(hIcmp);
#else
            (void)ips;
            for (int i = t; i < static_cast<int>(ips.size()); i += nThreads)
                online[i] = 0;
#endif
        });
    }
    for (auto& w : workers) w.join();

    // 按原顺序收集在线 IP
    for (int i = 0; i < total; ++i)
        if (online[i]) result.append(ips[i]);
    return result;
}

// ═════════════════════════════════════════════════════════════════════
// 虚拟采集卡探测（PALiveImagingSimSender）
//
// 向 127.0.0.1~127.0.0.4:8080 发送带探测标记的 58 字节配置命令，模拟器
// 在线时会从各虚拟卡源 IP 把 60 字节反馈回发到本探针的来源端口（不占用
// 8000，避免残留监听进程占用端口时探测失败）。仅用于真实网段扫描无结果
// 时的兜底识别，不影响真实采集卡流程。
// ═════════════════════════════════════════════════════════════════════
QVector<QString> NetworkController::scanVirtualCards()
{
    QVector<QString> result;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return result;
    }

    // 探测反馈监听：绑定临时端口接收模拟器回发的反馈
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port   = 0;
    local.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        closesocket(s);
        WSACleanup();
        return result;
    }

    DWORD timeout = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    // 58 字节配置命令探针：第 6 字节为探测标记 0xEE（正常配置命令该字节为 0）
    uint8_t cmd[58] = {};
    cmd[0] = 0xFA; cmd[1] = 0xFA; cmd[2] = 0xFA; cmd[3] = 0xFA;
    cmd[4] = 0x02;
    cmd[5] = 0xEE;
    for (int i = 0; i < 4; ++i) {
        sockaddr_in dst = {};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(CONTROL_PORT);
        inet_pton(AF_INET, QString("127.0.0.%1").arg(i + 1).toStdString().c_str(),
                  &dst.sin_addr);
        sendto(s, reinterpret_cast<const char*>(cmd), sizeof(cmd), 0,
               reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }

    std::vector<int> seen(5, 0);   // 下标 1~4 = 127.0.0.1~127.0.0.4
    const uint64_t deadline = nowMs() + 500;
    while (nowMs() < deadline) {
        char buf[128];
        sockaddr_in src = {};
        socklen_t srcLen = sizeof(src);
        const int n = recvfrom(s, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&src), &srcLen);
        if (n <= 0) continue;
        if (n != 60) continue;

        char ip[64];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
        const QStringList parts = QString(ip).split('.');
        if (parts.size() == 4 && parts[0] == "127" &&
            parts[1] == "0" && parts[2] == "0") {
            bool ok = false;
            const int last = parts[3].toInt(&ok);
            if (ok && last >= 1 && last <= 4) seen[last] = 1;
        }
    }
    for (int i = 1; i <= 4; ++i)
        if (seen[i]) result.append(QString("127.0.0.%1").arg(i));

    closesocket(s);
    WSACleanup();
#endif
    return result;
}
