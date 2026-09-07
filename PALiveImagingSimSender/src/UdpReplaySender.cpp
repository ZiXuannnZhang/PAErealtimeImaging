#include "UdpReplaySender.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cmath>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace {

constexpr int kUdpHeaderBytes = 4;
constexpr int kUdpPayloadBytes = 1440;
constexpr int kChannels = 8;

bool readColumn(std::ifstream &f, int sampDepth, long long col0,
                std::vector<double> &col) {
    col.resize(sampDepth);
    f.clear();
    f.seekg(static_cast<std::streamoff>(col0) * sampDepth * 8);
    f.read(reinterpret_cast<char *>(col.data()),
           static_cast<std::streamsize>(sampDepth * 8));
    return f.good() || f.gcount() == static_cast<std::streamsize>(sampDepth * 8);
}

// 确定性伪随机白噪声（xorshift64），幅度 ±amp
void genNoise(std::vector<double> &col, double amp, uint64_t seed) {
    uint64_t x = seed ? seed : 0x9E3779B97F4A7C15ULL;
    for (double &v : col) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const double u = static_cast<double>(x % 1000000ULL) / 1000000.0;
        v = (2.0 * u - 1.0) * amp;
    }
}

}  // namespace

UdpReplaySender::~UdpReplaySender() {
    requestStop();
    if (m_thread.joinable()) m_thread.join();
}

bool UdpReplaySender::start(const Config &cfg, Progress progress, Log log) {
    if (m_running.load() || cfg.dataPath.empty()) return false;
    // 上次发送线程可能已结束但尚未 join，重复 start() 时先回收，避免
    // std::thread 对 joinable 对象赋值导致 std::terminate 崩溃
    if (m_thread.joinable()) m_thread.join();
    m_cfg = cfg;
    // 通道开关补齐：未配置时默认 8 通道全开
    if (m_cfg.channelEnabled.size() < kChannels)
        m_cfg.channelEnabled.assign(kChannels, true);
    else if (m_cfg.channelEnabled.size() > kChannels)
        m_cfg.channelEnabled.resize(kChannels);
    m_progress = std::move(progress);
    m_log = std::move(log);
    m_stop.store(false);
    m_running.store(true);
    m_thread = std::thread([this] { run(); });
    return true;
}

void UdpReplaySender::requestStop() {
    m_stop.store(true);
}

void UdpReplaySender::run() {
    const int id = m_cfg.id;
    const int wlOffset = (id == 11) ? 151 : 301;
    const int nCards = 4;
    const int bytesPerSample = m_cfg.bits / 8;
    const int bytesPerPair = bytesPerSample * 2;
    const int bytesPerTrig = m_cfg.depth * bytesPerPair;
    const int packetsPerTrig =
        (bytesPerTrig + kUdpPayloadBytes - 1) / kUdpPayloadBytes;

    std::ifstream f(m_cfg.dataPath, std::ios::binary);
    if (!f) {
        if (m_log) m_log("无法打开数据文件: " + m_cfg.dataPath);
        m_running.store(false);
        return;
    }
    f.seekg(0, std::ios::end);
    const long long fileBytes = f.tellg();
    const long long totalCols =
        fileBytes / (static_cast<long long>(m_cfg.depth) * 8);
    const int nWlFrame = static_cast<int>((totalCols - (wlOffset - 1)) / 2);
    const int K0 = nWlFrame / 8;   // 源数据每通道每波长每圈A-line数（固定8通道扇区）

    // 发送通道数决定扇区聚合：勾选 C 个通道时，每个通道聚合 8/C 个源扇区，
    // 每通道每波长每圈 A-line 数变为 nWlFrame/C，对应“按勾选通道数均分 360°”。
    int nSignalCh = 0;
    for (int s = 0; s < kChannels; ++s)
        if (m_cfg.channelEnabled[s]) ++nSignalCh;
    if (nSignalCh == 0) nSignalCh = 8;   // 全部未勾选时按 8 通道发送
    if (8 % nSignalCh != 0) {
        if (m_log) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "勾选的发送通道数 %d 无法整除 8（支持 1/2/4/8），请调整通道勾选",
                          nSignalCh);
            m_log(buf);
        }
        m_running.store(false);
        return;
    }
    if (nWlFrame % nSignalCh != 0) {
        if (m_log) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "数据集每波长 %d 根无法按 %d 个发送通道整除，请调整通道勾选或数据集",
                          nWlFrame, nSignalCh);
            m_log(buf);
        }
        m_running.store(false);
        return;
    }
    const int K = nWlFrame / nSignalCh;  // 聚合后每通道每波长每圈A-line数
    const int nTriggers = m_cfg.triggers > 0 ? m_cfg.triggers : 2 * K;
    if (K0 < 2 || K < 2 || nTriggers < 2) {
        if (m_log) m_log("数据集列数与采样深度不匹配");
        m_running.store(false);
        return;
    }
    if (m_log) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "开始发送: K=%d triggers=%d bits=%d 端口=%d~%d 发送通道=%d（每通道聚合 %d 个扇区）",
                      K, nTriggers, m_cfg.bits, m_cfg.portBase,
                      m_cfg.portBase + nCards - 1, nSignalCh, 8 / nSignalCh);
        m_log(buf);
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SOCKET socks[4];
    sockaddr_in dst[4];
    for (int c = 0; c < nCards; ++c) {
        socks[c] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int sndbuf = 8 * 1024 * 1024;
        setsockopt(socks[c], SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char *>(&sndbuf), sizeof(sndbuf));
        dst[c] = {};
        dst[c].sin_family = AF_INET;
        dst[c].sin_port = htons(static_cast<u_short>(m_cfg.portBase + c));
        inet_pton(AF_INET, "127.0.0.1", &dst[c].sin_addr);
    }

    std::ifstream chStream[8];
    std::vector<double> col[8];
    std::vector<double> rms(8, 0.0);
    bool useSignal[8] = {};
    int streamGroupStart[8] = {};   // 勾选通道按物理号升序，第 i 个聚合源扇区起始
    const int groupSize = 8 / nSignalCh;
    {
        int idx = 0;
        for (int s = 0; s < kChannels; ++s) {
            streamGroupStart[s] = -1;
            if (m_cfg.channelEnabled[s]) {
                streamGroupStart[s] = idx * groupSize;
                ++idx;
            }
        }
    }

    // 聚合读取：勾选通道的第 g 个触发，映射回源数据对应扇区/通道/列
    auto readAggregated = [&](int srcGroupStart, int g, std::vector<double> &out) {
        if (srcGroupStart < 0 || srcGroupStart >= kChannels) return;
        const int lineIdxWl = g / 2;
        const int srcOff = lineIdxWl / K0;
        const int srcChan = srcGroupStart + srcOff;
        if (srcChan < 0 || srcChan >= kChannels) return;   // 防越界（末触发预读等场景）
        const int offInChan = (lineIdxWl % K0) * 2 + (g & 1);
        readColumn(chStream[srcChan], m_cfg.depth,
                   static_cast<long long>(wlOffset - 1) + 2LL * srcChan * K0 + offInChan,
                   out);
    };

    // 先把 8 个源通道文件流全部打开：聚合读取可能引用尚未轮到的源通道流，
    // 若边开边读，后置源通道未打开会导致首次装载失败（第0触发数据错误）。
    for (int s = 0; s < kChannels; ++s)
        chStream[s].open(m_cfg.dataPath, std::ios::binary);

    for (int s = 0; s < kChannels; ++s) {
        useSignal[s] = m_cfg.channelEnabled[s];
        readColumn(chStream[s], m_cfg.depth,
                   static_cast<long long>(wlOffset - 1) + 2LL * s * K0, col[s]);
        double sum2 = 0.0;
        for (const double v : col[s]) sum2 += v * v;
        rms[s] = std::sqrt(sum2 / static_cast<double>(m_cfg.depth));
        if (useSignal[s]) {
            readAggregated(streamGroupStart[s], 0, col[s]);
        } else {
            const double amp = rms[s] > 0.0 ? rms[s] * m_cfg.noisePercent / 100.0 : 1.0;
            genNoise(col[s], amp, 0x9E3779B97F4A7C15ULL *
                                     static_cast<uint64_t>(s + 1));
        }
    }

    std::vector<uint8_t> pkt(kUdpHeaderBytes + kUdpPayloadBytes);
    const int msPerTrig =
        m_cfg.rateHz > 0 ? static_cast<int>(1000.0 / m_cfg.rateHz) : 0;

    for (int g = 0; g < nTriggers && !m_stop.load(); ++g) {
        for (int c = 0; c < nCards; ++c) {
            const int chA = 2 * c;
            const int chB = 2 * c + 1;
            std::vector<uint8_t> payload(bytesPerTrig);
            for (int i = 0; i < m_cfg.depth; ++i) {
                const double vB = col[chB][i];
                const double vA = col[chA][i];
                if (m_cfg.bits == 16) {
                    int16_t b = static_cast<int16_t>(
                        vB < -32768.0 ? -32768.0 : (vB > 32767.0 ? 32767.0 : vB));
                    int16_t a = static_cast<int16_t>(
                        vA < -32768.0 ? -32768.0 : (vA > 32767.0 ? 32767.0 : vA));
                    std::memcpy(payload.data() + static_cast<size_t>(i) * 4, &b, 2);
                    std::memcpy(payload.data() + static_cast<size_t>(i) * 4 + 2, &a, 2);
                } else {
                    int32_t b = static_cast<int32_t>(
                        vB < -2147483647.0 ? -2147483647.0
                                           : (vB > 2147483647.0 ? 2147483647.0 : vB));
                    int32_t a = static_cast<int32_t>(
                        vA < -2147483647.0 ? -2147483647.0
                                           : (vA > 2147483647.0 ? 2147483647.0 : vA));
                    std::memcpy(payload.data() + static_cast<size_t>(i) * 8, &b, 4);
                    std::memcpy(payload.data() + static_cast<size_t>(i) * 8 + 4, &a, 4);
                }
            }

            for (int p = 0; p < packetsPerTrig; ++p) {
                const uint16_t packetSeq = static_cast<uint16_t>(
                    static_cast<uint64_t>(g) * packetsPerTrig + p);
                const uint16_t triggerSeq = static_cast<uint16_t>(g);
                pkt[0] = static_cast<uint8_t>(packetSeq & 0xFF);
                pkt[1] = static_cast<uint8_t>((packetSeq >> 8) & 0xFF);
                pkt[2] = static_cast<uint8_t>(triggerSeq & 0xFF);
                pkt[3] = static_cast<uint8_t>((triggerSeq >> 8) & 0xFF);
                const int off = p * kUdpPayloadBytes;
                const int len = std::min(kUdpPayloadBytes, bytesPerTrig - off);
                std::memcpy(pkt.data() + kUdpHeaderBytes, payload.data() + off, len);
                sendto(socks[c], reinterpret_cast<const char *>(pkt.data()),
                       kUdpHeaderBytes + len, 0,
                       reinterpret_cast<sockaddr *>(&dst[c]), sizeof(dst[c]));
            }
        }

        for (int s = 0; s < kChannels; ++s) {
            if (useSignal[s]) {
                // 末触发（g+1 == nTriggers）没有后续数据可读：跳过预读，
                // 避免聚合映射把源通道号推到 8（chStream 越界崩溃）
                if (g + 1 < nTriggers)
                    readAggregated(streamGroupStart[s], g + 1, col[s]);
            } else {
                const double amp = rms[s] > 0.0
                    ? rms[s] * m_cfg.noisePercent / 100.0 : 1.0;
                genNoise(col[s], amp,
                         0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(s + 1) +
                         0x2545F4914F6CDD1DULL * static_cast<uint64_t>(g + 1));
            }
        }

        if (msPerTrig > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(msPerTrig));
        if (m_progress) m_progress(g + 1, nTriggers);
    }

    for (int c = 0; c < nCards; ++c) closesocket(socks[c]);
    if (m_log) {
        m_log(m_stop.load() ? "发送已停止" : "发送完成");
    }
    m_running.store(false);
}
