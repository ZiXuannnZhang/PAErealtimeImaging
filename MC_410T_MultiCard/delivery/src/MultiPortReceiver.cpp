#include "MultiPortReceiver.h"
#include <QDebug>
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
    QObject* parent)
    : QThread(parent)
    , m_cardIndices(cardIndices)
    , m_processors(processors)
    , m_cpuCore(cpuCore)
{
    m_recvBuf.resize(RECV_BUF_SIZE);
    setObjectName(QString("MultiPortReceiver_%1").arg(cardIndices.empty() ? -1 : cardIndices[0]));
}

MultiPortReceiver::~MultiPortReceiver() {
    requestStop();
    if (!wait(2000)) {
        forceCloseSockets();   // 线程异常未退出时先关闭 socket，避免端口泄漏
        wait();   // 阻塞等待线程真正结束，避免“QThread destroyed while running”致命错误
    }
}

void MultiPortReceiver::requestStop() {
    m_running.store(false, std::memory_order_release);
    requestInterruption();
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

    if (!openSockets()) {
        emit errorOccurred("MultiPortReceiver: 打开 socket 失败");
        return;
    }

#ifdef _WIN32
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


#ifdef _WIN32
    while (m_running.load() && !isInterruptionRequested()) {
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
        if (ret <= 0) continue;  // timeout 或错误

        for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i) {
            SOCKET sock = static_cast<SOCKET>(m_sockets[i]);
            if (!FD_ISSET(sock, &readfds)) continue;

            // 排空式接收：一次 select 把该 socket 当前所有排队包全部读走，
            // 避免单包/轮询节奏在高触发率下形成接收滞后窗口
            sockaddr_in srcAddr{};
            int addrLen = sizeof(srcAddr);
            for (;;) {
                int recvd = recvfrom(sock, reinterpret_cast<char*>(m_recvBuf.data()),
                                     RECV_BUF_SIZE, 0,
                                     reinterpret_cast<sockaddr*>(&srcAddr), &addrLen);
                if (recvd == SOCKET_ERROR) break;   // WSAEWOULDBLOCK：已排空
                if (recvd < UDP_HEADER_BYTES) continue;

                // 解析包头：小端序（Little-Endian）
                // 字节0-1=packetSeq（低字节在前），字节2-3=triggerSeq（低字节在前）
                DataPacket pkt;
                const uint8_t* hdr = m_recvBuf.data();
                pkt.packetSeq  = static_cast<uint16_t>(hdr[0]) | (static_cast<uint16_t>(hdr[1]) << 8);
                pkt.triggerSeq = static_cast<uint16_t>(hdr[2]) | (static_cast<uint16_t>(hdr[3]) << 8);
                pkt.cardIndex  = m_cardIndices[i];
                pkt.dataSize   = static_cast<uint16_t>(
                    std::min(recvd - UDP_HEADER_BYTES, UDP_PAYLOAD_BYTES));
                std::memcpy(pkt.data, m_recvBuf.data() + UDP_HEADER_BYTES, pkt.dataSize);

                // 只有已经解析且确实存在对应 processor 时，才计入 socket 层；
                // 短包、停止唤醒包和无 processor 的数据都不冒充有效采集包。
                if (i < static_cast<int>(m_processors.size()) && m_processors[i]) {
                    m_processors[i]->stats().socketPacketsReceived.fetch_add(
                        1, std::memory_order_relaxed);
                    m_processors[i]->enqueuePacket(pkt);
                }
            }
        }
    }
#endif

    m_active.store(false);
    closeSockets();
    emit statusMessage("MultiPortReceiver 已停止");
}
