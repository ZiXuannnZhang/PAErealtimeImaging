#include "MultiPortReceiver.h"
#include <QDebug>
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
    if (!wait(500)) terminate();
}

void MultiPortReceiver::requestStop() {
    m_running.store(false, std::memory_order_release);
    requestInterruption();
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
            return false;
        }
        // 设置接收缓冲区 8MB
        int rcvbuf = 8 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<u_short>(port));
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            emit errorOccurred(QString("bind 失败，port=%1，错误=%2").arg(port).arg(WSAGetLastError()));
            closesocket(sock);
            return false;
        }
        m_sockets.push_back(static_cast<uintptr_t>(sock));
    }
    return true;
#else
    return false;
#endif
}

void MultiPortReceiver::closeSockets() {
#ifdef _WIN32
    for (auto s : m_sockets)
        closesocket(static_cast<SOCKET>(s));
    m_sockets.clear();
    WSACleanup();
#endif
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

            sockaddr_in srcAddr{};
            int addrLen = sizeof(srcAddr);
            int recvd = recvfrom(sock, reinterpret_cast<char*>(m_recvBuf.data()),
                                 RECV_BUF_SIZE, 0,
                                 reinterpret_cast<sockaddr*>(&srcAddr), &addrLen);
            if (recvd < UDP_HEADER_BYTES) continue;

            // 解析包头：小端序（Little-Endian）
            // 字节0-1=packetSeq（低字节在前），字节2-3=triggerSeq（低字节在前）
            // 如 [01 00 00 00] = packetSeq=1, triggerSeq=0
            DataPacket pkt;
            const uint8_t* hdr = m_recvBuf.data();
            pkt.packetSeq  = static_cast<uint16_t>(hdr[0]) | (static_cast<uint16_t>(hdr[1]) << 8);
            pkt.triggerSeq = static_cast<uint16_t>(hdr[2]) | (static_cast<uint16_t>(hdr[3]) << 8);
            pkt.cardIndex  = m_cardIndices[i];
            pkt.dataSize   = static_cast<uint16_t>(
                std::min(recvd - UDP_HEADER_BYTES, UDP_PAYLOAD_BYTES));
            std::memcpy(pkt.data, m_recvBuf.data() + UDP_HEADER_BYTES, pkt.dataSize);

            // 交给对应的 DataProcessor
            if (i < static_cast<int>(m_processors.size()) && m_processors[i])
                m_processors[i]->enqueuePacket(pkt);
        }
    }
#endif

    m_active.store(false);
    closeSockets();
    emit statusMessage("MultiPortReceiver 已停止");
}
