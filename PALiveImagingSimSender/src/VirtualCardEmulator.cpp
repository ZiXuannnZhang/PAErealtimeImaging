#include "VirtualCardEmulator.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace {

constexpr int kReadyPacketBytes = 18;
constexpr int kConfigAckBytes   = 60;
constexpr int kControlPacketBytes = 58;
constexpr uint8_t kMagic[4] = {0xFA, 0xFA, 0xFA, 0xFA};
// 监听程序网段扫描用的探测命令标记（放在配置命令第 6 字节，正常配置命令为 0）
constexpr uint8_t kProbeMarker = 0xEE;

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

VirtualCardEmulator::~VirtualCardEmulator() {
    requestStop();
    if (m_thread.joinable()) m_thread.join();
}

bool VirtualCardEmulator::start(const Config &cfg, Log log) {
    if (m_running.load()) return false;
    // 上次虚拟采集卡线程可能已结束但尚未 join，重复 start() 时先回收，
    // 避免 std::thread 对 joinable 对象赋值导致 std::terminate 崩溃
    if (m_thread.joinable()) m_thread.join();
    m_cfg = cfg;
    if (m_cfg.nCards < 1) m_cfg.nCards = 1;
    if (m_cfg.nCards > 4) m_cfg.nCards = 4;
    m_log = std::move(log);
    m_stop.store(false);
    m_running.store(true);
    m_thread = std::thread([this] { run(); });
    return true;
}

void VirtualCardEmulator::requestStop() {
    m_stop.store(true);
}

std::string VirtualCardEmulator::cardSourceIP(int cardIdx) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "127.0.0.%d", cardIdx + 1);
    return buf;
}

void VirtualCardEmulator::sendReady(int cardIdx, bool announce) {
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    inet_pton(AF_INET, cardSourceIP(cardIdx).c_str(), &local.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        closesocket(s);
        return;
    }

    uint8_t pkt[kReadyPacketBytes] = {};
    std::memcpy(pkt, kMagic, sizeof(kMagic));
    pkt[4] = 0x01;                    // 版本号/就绪包
    pkt[5] = static_cast<uint8_t>(cardIdx);

    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(m_cfg.feedbackPort));
    inet_pton(AF_INET, m_cfg.hostIP.c_str(), &dst.sin_addr);
    const int n = sendto(s, reinterpret_cast<const char *>(pkt), sizeof(pkt), 0,
                         reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    closesocket(s);

    if (announce && n > 0 && m_log) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "[虚拟卡] 卡%d（%s）→ %s:%d 已发送 18 字节就绪包",
                      cardIdx + 1, cardSourceIP(cardIdx).c_str(),
                      m_cfg.hostIP.c_str(), m_cfg.feedbackPort);
        m_log(buf);
    }
#endif
}

void VirtualCardEmulator::sendConfigAck(int cardIdx) {
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    inet_pton(AF_INET, cardSourceIP(cardIdx).c_str(), &local.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        closesocket(s);
        return;
    }

    uint8_t ack[kConfigAckBytes] = {};
    std::memcpy(ack, kMagic, sizeof(kMagic));
    ack[4] = 0x02;                    // 配置反馈
    ack[5] = static_cast<uint8_t>(cardIdx);
    ack[kConfigAckBytes - 1] = 0x01;  // 成功标志

    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(m_cfg.feedbackPort));
    inet_pton(AF_INET, m_cfg.hostIP.c_str(), &dst.sin_addr);
    sendto(s, reinterpret_cast<const char *>(ack), sizeof(ack), 0,
           reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    closesocket(s);
#endif
}

// 探测应答：把 60 字节反馈直接回发到控制命令的来源地址（不依赖固定 8000 端口）
void VirtualCardEmulator::sendAckTo(int cardIdx, const sockaddr_in &sender) {
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    inet_pton(AF_INET, cardSourceIP(cardIdx).c_str(), &local.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        closesocket(s);
        return;
    }

    uint8_t ack[kConfigAckBytes] = {};
    std::memcpy(ack, kMagic, sizeof(kMagic));
    ack[4] = 0x02;
    ack[5] = static_cast<uint8_t>(cardIdx);
    ack[kConfigAckBytes - 1] = 0x01;
    sendto(s, reinterpret_cast<const char *>(ack), sizeof(ack), 0,
           reinterpret_cast<const sockaddr *>(&sender), sizeof(sender));
    closesocket(s);
#endif
}

void VirtualCardEmulator::run() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET ctrl = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctrl == INVALID_SOCKET) {
        if (m_log) m_log("[虚拟卡] 创建控制 socket 失败");
        m_running.store(false);
        WSACleanup();
        return;
    }

    // 200ms 接收超时，使线程能及时响应停止请求
    DWORD timeout = 200;
    setsockopt(ctrl, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout), sizeof(timeout));

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<uint16_t>(m_cfg.controlPort));
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(ctrl, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        if (m_log) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "[虚拟卡] 启动失败：控制端口 %d 绑定失败（WSA=%d），请检查端口占用",
                          m_cfg.controlPort, WSAGetLastError());
            m_log(buf);
        }
        closesocket(ctrl);
        m_running.store(false);
        WSACleanup();
        return;
    }

    if (m_log) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "[虚拟卡] 已启动 %d 张卡，源IP %s~%s，控制端口 %d，反馈目标 %s:%d",
                      m_cfg.nCards, cardSourceIP(0).c_str(),
                      cardSourceIP(m_cfg.nCards - 1).c_str(),
                      m_cfg.controlPort, m_cfg.hostIP.c_str(),
                      m_cfg.feedbackPort);
        m_log(buf);
    }

    // 启动时连续发送 3 轮就绪包，保证监听程序即使后启动也能立刻检测到
    for (int round = 0; round < 3 && !m_stop.load(); ++round) {
        for (int c = 0; c < m_cfg.nCards; ++c) sendReady(c, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    uint64_t lastReadyMs = nowMs();
    char buf[256];
    sockaddr_in sender = {};
    socklen_t senderLen = sizeof(sender);

    while (!m_stop.load()) {
        // ── 周期性就绪包 ──────────────────────────────────────
        const uint64_t now = nowMs();
        if (now - lastReadyMs >= static_cast<uint64_t>(m_cfg.readyIntervalMs)) {
            lastReadyMs = now;
            for (int c = 0; c < m_cfg.nCards; ++c) sendReady(c, false);
        }

        // ── 控制命令接收与应答 ────────────────────────────────
        const int n = recvfrom(ctrl, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr *>(&sender), &senderLen);
        if (n <= 0) continue;

        const uint8_t *p = reinterpret_cast<const uint8_t *>(buf);
        if (n >= 4 && std::memcmp(p, kMagic, sizeof(kMagic)) == 0) {
            char src[64];
            inet_ntop(AF_INET, &sender.sin_addr, src, sizeof(src));
            if (n == kControlPacketBytes && (p[4] == 0x02 || p[4] == 0x03)) {
                const char *type = (p[4] == 0x02) ? "配置参数(0x02)" : "测量控制(0x03)";
                if (m_log) {
                    char msg[192];
                    std::snprintf(msg, sizeof(msg),
                                  "[虚拟卡] 收到 %s 命令（%d 字节，来自 %s:%d）",
                                  type, n, src, ntohs(sender.sin_port));
                    m_log(msg);
                }
                if (p[4] == 0x02) {
                    if (p[5] == kProbeMarker) {
                        // 探测命令：向来源地址回发反馈（监听程序扫描用）
                        for (int c = 0; c < m_cfg.nCards; ++c)
                            sendAckTo(c, sender);
                        if (m_log) {
                            char msg[160];
                            std::snprintf(msg, sizeof(msg),
                                          "[虚拟卡] 已应答探测（%d 张卡，回发 %s:%d）",
                                          m_cfg.nCards, src, ntohs(sender.sin_port));
                            m_log(msg);
                        }
                    } else {
                        // 正常配置命令：每张卡独立回发 60 字节反馈到监听程序 8000
                        for (int c = 0; c < m_cfg.nCards; ++c) {
                            sendConfigAck(c);
                            sendReady(c, false);
                        }
                        if (m_log) {
                            char msg[160];
                            std::snprintf(msg, sizeof(msg),
                                          "[虚拟卡] 已回发 %d 张卡 60 字节配置反馈",
                                          m_cfg.nCards);
                            m_log(msg);
                        }
                    }
                } else {
                    const char *act = (n > 5 && p[n - 1] == 0x01) ? "开始测量" : "停止测量";
                    if (m_log) {
                        char msg[128];
                        std::snprintf(msg, sizeof(msg), "[虚拟卡] 收到 %s 命令", act);
                        m_log(msg);
                    }
                }
            } else {
                if (m_log) {
                    char msg[128];
                    std::snprintf(msg, sizeof(msg),
                                  "[虚拟卡] 忽略未知控制包（%d 字节，来自 %s）", n, src);
                    m_log(msg);
                }
            }
        }
        senderLen = sizeof(sender);
    }

    closesocket(ctrl);
    if (m_log) m_log("[虚拟卡] 已停止");
    m_running.store(false);
    WSACleanup();
#endif
}
