#include "MultiPortReceiver.h"
#include <QDebug>
#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#endif

MultiPortReceiver::MultiPortReceiver(
    const std::vector<int>& cardIndices,
    const std::vector<DataProcessor*>& processors,
    int cpuCore,
    QObject* parent,
    bool testNoSockets)
    : QThread(parent)
    , m_cardIndices(cardIndices)
    , m_processors(processors)
    , m_cpuCore(cpuCore)
    , m_testNoSockets(testNoSockets)
{
    m_recvBuf.resize(RECV_BUF_SIZE);
    for (size_t i = 0; i < m_cardAdmissionStates.size(); ++i) {
        m_cardAdmissionStates[i].store(static_cast<int>(AdmissionState::Disarmed),
                                       std::memory_order_relaxed);
        m_cardSessionTokens[i].store(0, std::memory_order_relaxed);
    }
    setObjectName(QString("MultiPortReceiver_%1").arg(cardIndices.empty() ? -1 : cardIndices[0]));
}

MultiPortReceiver::~MultiPortReceiver() {
    requestStop();
    if (!wait(2000)) {
        forceCloseSockets();   // 线程异常未退出时先关闭 socket，避免端口泄漏
        wait();   // 阻塞等待线程真正结束，避免“QThread destroyed while running”致命错误
    }
}

void MultiPortReceiver::updateMax(std::atomic<uint64_t>& target, uint64_t value)
{
    uint64_t previous = target.load(std::memory_order_relaxed);
    while (previous < value
           && !target.compare_exchange_weak(previous, value,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
    }
}

MultiPortReceiver::ObservabilitySnapshot MultiPortReceiver::observabilitySnapshot() const
{
    ObservabilitySnapshot snapshot;
    snapshot.cardIndices = m_cardIndices;
    snapshot.selectWakeups = m_selectWakeups.load(std::memory_order_relaxed);
    snapshot.selectTimeouts = m_selectTimeouts.load(std::memory_order_relaxed);
    snapshot.selectErrors = m_selectErrors.load(std::memory_order_relaxed);
    snapshot.recvHardErrors = m_recvHardErrors.load(std::memory_order_relaxed);
    snapshot.recvWouldBlockTerminations = m_recvWouldBlockTerminations.load(std::memory_order_relaxed);
    snapshot.maxDrainPackets = m_maxDrainPackets.load(std::memory_order_relaxed);
    snapshot.maxDrainDurationUs = m_maxDrainDurationUs.load(std::memory_order_relaxed);
    snapshot.maxReceiverLoopGapUs = m_maxReceiverLoopGapUs.load(std::memory_order_relaxed);
    snapshot.sockets.reserve(m_cardIndices.size());
    for (size_t i = 0; i < m_cardIndices.size(); ++i) {
        SocketObservabilitySnapshot socket;
        socket.cardIndex = m_cardIndices[i];
        if (i < m_socketMaxDrainPackets.size())
            socket.maxDrainPackets = m_socketMaxDrainPackets[i].load(std::memory_order_relaxed);
        snapshot.sockets.push_back(socket);
    }
    return snapshot;
}

void MultiPortReceiver::requestStop() {
    m_running.store(false, std::memory_order_release);
    requestInterruption();
    m_commandCv.notify_all();
#ifdef _WIN32
    // 立即唤醒 select/recvfrom：向本机各端口发送空 UDP 包，
    // 避免线程停在 select 上直到超时，导致 stop() 走 terminate 泄漏端口。
    std::lock_guard<std::mutex> lk(m_socketMutex);
    for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i) {
        SOCKET sock = static_cast<SOCKET>(m_sockets[i]);
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(static_cast<u_short>(BASE_PORT + m_cardIndices[i]));
        dst.sin_addr.s_addr = inet_addr("127.0.0.1");
        sendto(sock, "", 0, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }
#endif
}

bool MultiPortReceiver::prepareSession(uint64_t sessionToken, int timeoutMs)
{
    return sessionToken != 0
        && postSessionCommand(SessionCommandKind::Prepare, sessionToken, -1, timeoutMs);
}

bool MultiPortReceiver::armSession(uint64_t sessionToken, int timeoutMs)
{
    return sessionToken != 0
        && postSessionCommand(SessionCommandKind::Arm, sessionToken, -1, timeoutMs);
}

bool MultiPortReceiver::commitCardSession(uint64_t sessionToken,
                                          int cardIndex,
                                          int timeoutMs)
{
    const bool ownsCard = std::find(m_cardIndices.begin(), m_cardIndices.end(), cardIndex)
                          != m_cardIndices.end();
    return sessionToken != 0 && cardIndex >= 0 && ownsCard
        && postSessionCommand(SessionCommandKind::CommitCard, sessionToken,
                              cardIndex, timeoutMs);
}

bool MultiPortReceiver::disarmSession(int timeoutMs)
{
    return postSessionCommand(SessionCommandKind::Disarm, 0, -1, timeoutMs);
}

void MultiPortReceiver::setCompatibilityAdmission(bool enable,
                                                   uint64_t sessionToken)
{
    if (enable && sessionToken != 0) {
        m_sessionToken.store(sessionToken, std::memory_order_release);
        for (size_t i = 0; i < m_cardIndices.size() && i < m_cardAdmissionStates.size(); ++i) {
            m_cardSessionTokens[i].store(sessionToken, std::memory_order_release);
            m_cardAdmissionStates[i].store(static_cast<int>(AdmissionState::Running),
                                           std::memory_order_release);
        }
        m_admissionState.store(static_cast<int>(AdmissionState::Running),
                               std::memory_order_release);
    } else {
        m_sessionToken.store(0, std::memory_order_release);
        for (size_t i = 0; i < m_cardIndices.size() && i < m_cardAdmissionStates.size(); ++i) {
            m_cardSessionTokens[i].store(0, std::memory_order_release);
            m_cardAdmissionStates[i].store(static_cast<int>(AdmissionState::Disarmed),
                                           std::memory_order_release);
        }
        m_admissionState.store(static_cast<int>(AdmissionState::Disarmed),
                               std::memory_order_release);
    }
}

bool MultiPortReceiver::postSessionCommand(SessionCommandKind kind,
                                           uint64_t sessionToken,
                                           int cardIndex,
                                           int timeoutMs)
{
    if (!m_active.load(std::memory_order_acquire)) return false;
    auto wait = std::make_shared<SessionCommandWait>();
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        m_commands.push_back(SessionCommand{kind, sessionToken, cardIndex,
                                            timeoutMs, wait});
    }
    m_commandCv.notify_one();
    std::unique_lock<std::mutex> lock(wait->mutex);
    const auto ready = [&wait] { return wait->done; };
    const bool reached = timeoutMs < 0
        ? (wait->condition.wait(lock, ready), true)
        : wait->condition.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
    return reached && wait->success;
}

void MultiPortReceiver::processSessionCommands()
{
    std::deque<SessionCommand> commands;
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        commands.swap(m_commands);
    }
    for (const SessionCommand& command : commands) {
        const bool success = applySessionCommand(command);
        if (!command.wait) continue;
        std::lock_guard<std::mutex> lock(command.wait->mutex);
        command.wait->success = success;
        command.wait->done = true;
        command.wait->condition.notify_one();
    }
}

bool MultiPortReceiver::applySessionCommand(const SessionCommand& command)
{
    switch (command.kind) {
    case SessionCommandKind::Prepare:
        // Close admission before quiescing so packets arriving while the
        // receiver drains the old socket backlog can never enter the next
        // processor session or raw-order anchor.
        m_admissionState.store(static_cast<int>(AdmissionState::Preparing),
                               std::memory_order_release);
        m_sessionToken.store(0, std::memory_order_release);
        for (size_t i = 0; i < m_cardIndices.size() && i < m_cardAdmissionStates.size(); ++i) {
            m_cardSessionTokens[i].store(0, std::memory_order_release);
            m_cardAdmissionStates[i].store(static_cast<int>(AdmissionState::Preparing),
                                           std::memory_order_release);
        }
        if (!drainSocketBacklog(command.timeoutMs)) {
            m_admissionState.store(static_cast<int>(AdmissionState::Disarmed),
                                   std::memory_order_release);
            for (size_t i = 0; i < m_cardIndices.size() && i < m_cardAdmissionStates.size(); ++i)
                m_cardAdmissionStates[i].store(static_cast<int>(AdmissionState::Disarmed),
                                               std::memory_order_release);
            return false;
        }
        return true;
    case SessionCommandKind::Arm:
        if (command.sessionToken == 0 ||
            !drainSocketBacklog(command.timeoutMs))
            return false;
        // ARMED is deliberately still closed to datagram admission.  The
        // controller opens each card only after that card's hardware Start
        // send succeeds.
        m_sessionToken.store(command.sessionToken, std::memory_order_release);
        for (size_t i = 0; i < m_cardIndices.size() && i < m_cardAdmissionStates.size(); ++i) {
            m_cardSessionTokens[i].store(command.sessionToken, std::memory_order_release);
            m_cardAdmissionStates[i].store(static_cast<int>(AdmissionState::Armed),
                                           std::memory_order_release);
        }
        m_admissionState.store(static_cast<int>(AdmissionState::Armed),
                               std::memory_order_release);
        return true;
    case SessionCommandKind::CommitCard: {
        int localIndex = -1;
        for (int i = 0; i < static_cast<int>(m_cardIndices.size()); ++i) {
            if (m_cardIndices[static_cast<size_t>(i)] == command.cardIndex) {
                localIndex = i;
                break;
            }
        }
        if (command.sessionToken == 0 || localIndex < 0 ||
            localIndex >= static_cast<int>(m_cardAdmissionStates.size()) ||
            m_cardSessionTokens[static_cast<size_t>(localIndex)].load(std::memory_order_acquire)
                != command.sessionToken ||
            m_cardAdmissionStates[static_cast<size_t>(localIndex)].load(std::memory_order_acquire)
                != static_cast<int>(AdmissionState::Armed))
            return false;
        m_cardAdmissionStates[static_cast<size_t>(localIndex)].store(
            static_cast<int>(AdmissionState::Running), std::memory_order_release);
        updateAggregateAdmissionState();
        return true;
    }
    case SessionCommandKind::Disarm:
        m_admissionState.store(static_cast<int>(AdmissionState::Disarmed),
                               std::memory_order_release);
        m_sessionToken.store(0, std::memory_order_release);
        for (size_t i = 0; i < m_cardIndices.size() && i < m_cardAdmissionStates.size(); ++i) {
            m_cardSessionTokens[i].store(0, std::memory_order_release);
            m_cardAdmissionStates[i].store(static_cast<int>(AdmissionState::Disarmed),
                                           std::memory_order_release);
        }
        return drainSocketBacklog(command.timeoutMs);
    }
    return false;
}

void MultiPortReceiver::updateAggregateAdmissionState()
{
    if (m_cardIndices.empty()) {
        m_admissionState.store(static_cast<int>(AdmissionState::Disarmed),
                               std::memory_order_release);
        return;
    }
    bool allRunning = true;
    bool anyPreparing = false;
    bool anyArmed = false;
    for (size_t i = 0; i < m_cardIndices.size() && i < m_cardAdmissionStates.size(); ++i) {
        const int state = m_cardAdmissionStates[i].load(std::memory_order_acquire);
        allRunning = allRunning && state == static_cast<int>(AdmissionState::Running);
        anyPreparing = anyPreparing || state == static_cast<int>(AdmissionState::Preparing);
        anyArmed = anyArmed || state == static_cast<int>(AdmissionState::Armed);
    }
    const AdmissionState aggregate = allRunning
        ? AdmissionState::Running
        : (anyPreparing ? AdmissionState::Preparing
                       : (anyArmed ? AdmissionState::Armed : AdmissionState::Disarmed));
    m_admissionState.store(static_cast<int>(aggregate), std::memory_order_release);
}

bool MultiPortReceiver::drainSocketBacklog(int timeoutMs)
{
    if (m_testNoSockets) return true;
#ifdef _WIN32
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeoutMs));
    for (;;) {
        bool anyData = false;
        for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i) {
            uint64_t drained = 0;
            const bool ok = drainSocket(m_sockets[i], i, true, &drained);
            if (!ok) return false;
            anyData = anyData || drained != 0;
        }
        if (!anyData) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
    }
#else
    Q_UNUSED(timeoutMs);
    return false;
#endif
}

bool MultiPortReceiver::dispatchDatagramForTest(const QByteArray& datagram,
                                                int socketIndex)
{
    return dispatchDatagram(datagram.constData(), datagram.size(), socketIndex);
}

bool MultiPortReceiver::dispatchDatagram(const char* bytes, int length, int socketIndex)
{
    if (!bytes || length < UDP_HEADER_BYTES ||
        socketIndex < 0 || socketIndex >= static_cast<int>(m_processors.size()) ||
        socketIndex >= static_cast<int>(m_cardIndices.size()) ||
        !m_processors[socketIndex])
        return false;

    DataPacket pkt;
    const auto* hdr = reinterpret_cast<const uint8_t*>(bytes);
    pkt.packetSeq = static_cast<uint16_t>(hdr[0]) |
                    (static_cast<uint16_t>(hdr[1]) << 8);
    pkt.triggerSeq = static_cast<uint16_t>(hdr[2]) |
                     (static_cast<uint16_t>(hdr[3]) << 8);
    pkt.cardIndex = m_cardIndices[socketIndex];
    pkt.dataSize = static_cast<uint16_t>(
        std::min(length - UDP_HEADER_BYTES, UDP_PAYLOAD_BYTES));
    std::memcpy(pkt.data, bytes + UDP_HEADER_BYTES, pkt.dataSize);

    DataProcessor* processor = m_processors[socketIndex];
    processor->stats().socketPacketsReceived.fetch_add(1, std::memory_order_relaxed);
    const AdmissionState state = socketIndex < static_cast<int>(m_cardAdmissionStates.size())
        ? static_cast<AdmissionState>(m_cardAdmissionStates[static_cast<size_t>(socketIndex)]
                                          .load(std::memory_order_acquire))
        : AdmissionState::Disarmed;
    const uint64_t token = socketIndex < static_cast<int>(m_cardSessionTokens.size())
        ? m_cardSessionTokens[static_cast<size_t>(socketIndex)].load(std::memory_order_acquire)
        : 0;
    if (state != AdmissionState::Running || token == 0) {
        processor->stats().sessionBoundaryPacketsDiscarded.fetch_add(
            1, std::memory_order_relaxed);
        return true;
    }

    // Raw-order observation and queue admission are one receiver-owned
    // decision.  A trailing packet cannot establish a new raw anchor because
    // observeRawReceive is reached only after the Running admission check.
    processor->stats().observeRawReceive(pkt.triggerSeq, pkt.packetSeq);
    processor->enqueuePacketForSession(pkt, token);
    return true;
}

bool MultiPortReceiver::drainSocket(uintptr_t socketValue,
                                    int socketIndex,
                                    bool dispatch,
                                    uint64_t* outPackets)
{
#ifdef _WIN32
    SOCKET sock = static_cast<SOCKET>(socketValue);
    sockaddr_in srcAddr{};
    uint64_t drainPackets = 0;
    const auto drainStart = std::chrono::steady_clock::now();
    for (;;) {
        int addrLen = sizeof(srcAddr);
        const int recvd = recvfrom(sock,
                                   reinterpret_cast<char*>(m_recvBuf.data()),
                                   RECV_BUF_SIZE, 0,
                                   reinterpret_cast<sockaddr*>(&srcAddr), &addrLen);
        if (recvd == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
                m_recvWouldBlockTerminations.fetch_add(1, std::memory_order_relaxed);
            else {
                m_recvHardErrors.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            break;
        }
        ++drainPackets;
        if (dispatch)
            dispatchDatagram(reinterpret_cast<const char*>(m_recvBuf.data()),
                             recvd, socketIndex);
    }
    const uint64_t drainDurationUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - drainStart).count());
    updateMax(m_maxDrainPackets, drainPackets);
    updateMax(m_maxDrainDurationUs, drainDurationUs);
    if (socketIndex >= 0 && socketIndex < static_cast<int>(m_socketMaxDrainPackets.size()))
        updateMax(m_socketMaxDrainPackets[socketIndex], drainPackets);
    if (outPackets) *outPackets = drainPackets;
    return true;
#else
    Q_UNUSED(socketValue);
    Q_UNUSED(socketIndex);
    Q_UNUSED(dispatch);
    return false;
#endif
}

bool MultiPortReceiver::openSockets() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    for (int i = 0; i < static_cast<int>(m_cardIndices.size()); ++i) {
        int port = BASE_PORT + m_cardIndices[i];
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            emit errorOccurred(QString("创建 socket 失败，port=%1").arg(port));
            closeSockets();   // 清理已成功打开的 socket，避免泄漏
            return false;
        }
        // 非阻塞模式：排空式 recvfrom 读到 WSAEWOULDBLOCK 即结束本轮
        u_long nonblock = 1;
        ioctlsocket(sock, FIONBIO, &nonblock);

        // 设置接收缓冲区 32MB（100Hz×4卡≈13MB/s，缓冲可吸收瞬时调度抖动不丢包）
        int rcvbuf = 32 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
        int rcvActual = 0;
        socklen_t rcvLen = sizeof(rcvActual);
        getsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<char*>(&rcvActual), &rcvLen);
        emit statusMessage(QString("卡%1 接收缓冲实际=%2 KB")
                           .arg(i + 1).arg(rcvActual / 1024));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<u_short>(port));
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            emit errorOccurred(QString("bind 失败，port=%1，错误=%2").arg(port).arg(WSAGetLastError()));
            closesocket(sock);
            closeSockets();   // 清理已成功打开的 socket，避免后续 start 反复 bind 失败
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(m_socketMutex);
            m_sockets.push_back(static_cast<uintptr_t>(sock));
        }
    }
    return true;
#else
    return false;
#endif
}

void MultiPortReceiver::closeSockets() {
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lk(m_socketMutex);
        for (auto s : m_sockets)
            closesocket(static_cast<SOCKET>(s));
        m_sockets.clear();
    }
    WSACleanup();
#endif
}

void MultiPortReceiver::forceCloseSockets() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(m_socketMutex);
    for (auto s : m_sockets)
        closesocket(static_cast<SOCKET>(s));
    m_sockets.clear();
#endif
}

bool MultiPortReceiver::waitUntilStarted(int timeoutMs) const {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (m_active.load(std::memory_order_acquire)) return true;
        // 线程已退出但未进入接收循环 → 端口绑定失败
        if (!isRunning() && !m_active.load(std::memory_order_acquire)) return false;
        QThread::msleep(5);
    }
    return m_active.load(std::memory_order_acquire);
}

void MultiPortReceiver::run() {
    m_running.store(true);

    if (!m_testNoSockets && !openSockets()) {
        emit errorOccurred("MultiPortReceiver: 打开 socket 失败");
        return;
    }

#ifdef _WIN32
    auto previousLoopStart = std::chrono::steady_clock::now();
    // CPU 亲和性设置
    if (m_cpuCore >= 0) {
        DWORD_PTR mask = 1ULL << m_cpuCore;
        SetThreadAffinityMask(GetCurrentThread(), mask);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif

    m_active.store(true);
    emit statusMessage(QString("MultiPortReceiver 启动：%1 个端口，绑定核心 %2")
                       .arg(m_cardIndices.size()).arg(m_cpuCore));


    while (m_running.load() && !isInterruptionRequested()) {
        processSessionCommands();
        if (m_testNoSockets) {
            std::unique_lock<std::mutex> lock(m_commandMutex);
            m_commandCv.wait_for(lock, std::chrono::milliseconds(1), [this] {
                return !m_commands.empty() || !m_running.load() ||
                       isInterruptionRequested();
            });
            continue;
        }
#ifdef _WIN32
        const auto loopStart = std::chrono::steady_clock::now();
        updateMax(m_maxReceiverLoopGapUs,
                  static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                      loopStart - previousLoopStart).count()));
        previousLoopStart = loopStart;
        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = 0;
        for (auto s : m_sockets) {
            FD_SET(static_cast<SOCKET>(s), &readfds);
            if (static_cast<int>(s) > maxfd) maxfd = static_cast<int>(s);
        }

        timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = SELECT_TIMEOUT_US;

        int ret = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
        if (ret == 0) {
            m_selectTimeouts.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (ret < 0) {
            m_selectErrors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        m_selectWakeups.fetch_add(1, std::memory_order_relaxed);

        for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i) {
            SOCKET sock = static_cast<SOCKET>(m_sockets[i]);
            if (!FD_ISSET(sock, &readfds)) continue;
            drainSocket(static_cast<uintptr_t>(sock), i, true);
        }
#endif
    }

    m_active.store(false);
    m_admissionState.store(static_cast<int>(AdmissionState::Disarmed),
                           std::memory_order_release);
    m_sessionToken.store(0, std::memory_order_release);
    for (size_t i = 0; i < m_cardIndices.size() && i < m_cardAdmissionStates.size(); ++i) {
        m_cardAdmissionStates[i].store(static_cast<int>(AdmissionState::Disarmed),
                                       std::memory_order_release);
        m_cardSessionTokens[i].store(0, std::memory_order_release);
    }
    if (!m_testNoSockets) closeSockets();
    emit statusMessage("MultiPortReceiver 已停止");
}
