// ring_udp_replay — 同机 UDP 回放器（M3 阶段 B 联调）
//
// 把已解调的 11.dat/14.dat 时域数据按真实采集 UDP 协议回放到本机：
//   - 每卡一个端口：BASE_PORT(8001) + cardIndex
//   - 包格式：4 字节头(packetSeq LE, triggerSeq LE) + 载荷(≤1440B)
//   - 载荷为 B/A 交织采样对（16bit: B(2B)+A(2B)；32bit: B(4B)+A(4B)）
//   - 触发 g：8 通道同步，通道 s 取文件列 wlOffset-1+2*s*K+g
//     （即按角度扇区连续切分的同一套映射）
//   - 载荷已是解调后时域信号，接收端须启用 PayloadIsTimeDomain 跳过频率换算
//
// 用法：
//   ring_udp_replay --data D:\zzx\data\20260519\14.dat --id 14
//                    [--depth 4000] [--bits 32] [--rate 0] [--triggers 0]

#include "Constants.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// 精确节拍：timeBeginPeriod(1) 把系统定时器精度提到 1ms，Sleep(1) 不再过量，
// 保证 --rate 100 真实达到 100Hz，且不占用 CPU 自旋（避免挤压接收进程）。
static void preciseSleepMs(int ms) {
    if (ms <= 0) return;
    timeBeginPeriod(1);
    Sleep(ms);
    timeEndPeriod(1);
}
#endif

namespace {

std::string argStr(const std::vector<std::string>& args, const std::string& key,
                   const std::string& def = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return args[i + 1];
    return def;
}
int argInt(const std::vector<std::string>& args, const std::string& key, int def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return std::stoi(args[i + 1]);
    return def;
}

bool readColumn(std::ifstream& f, int sampDepth, long long col0,
                std::vector<double>& col) {
    col.resize(sampDepth);
    f.clear();
    f.seekg(static_cast<std::streamoff>(col0) * sampDepth * 8);
    f.read(reinterpret_cast<char*>(col.data()),
           static_cast<std::streamsize>(sampDepth * 8));
    return f.good() || f.gcount() == static_cast<std::streamsize>(sampDepth * 8);
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string dataPath = argStr(args, "--data");
    const int id = argInt(args, "--id", 14);
    const int depth = argInt(args, "--depth", 4000);
    const int bits = argInt(args, "--bits", 32);
    const double rateHz = argInt(args, "--rate", 0);
    const int triggersOverride = argInt(args, "--triggers", 0);
    const int portBase = argInt(args, "--port-base", BASE_PORT);

    if (dataPath.empty()) {
        std::fprintf(stderr,
            "usage: ring_udp_replay --data <14.dat> [--id 14] [--depth 4000] "
            "[--bits 16|32] [--rate 0] [--triggers 0]\n");
        return 2;
    }
    if (bits != 16 && bits != 32) {
        std::fprintf(stderr, "--bits must be 16 or 32\n");
        return 2;
    }

    const int wlOffset = (id == 11) ? 151 : 301;
    const int nCards = 4;
    const int bytesPerSample = bits / 8;
    const int bytesPerPair = bytesPerSample * 2;
    const int pairsPerPkt = UDP_PAYLOAD_BYTES / bytesPerPair;
    const int bytesPerTrig = depth * bytesPerPair;
    const int packetsPerTrig =
        (bytesPerTrig + UDP_PAYLOAD_BYTES - 1) / UDP_PAYLOAD_BYTES;

    std::ifstream f(dataPath, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "open %s failed\n", dataPath.c_str());
        return 2;
    }
    f.seekg(0, std::ios::end);
    const long long fileBytes = f.tellg();
    const long long totalCols = fileBytes / (static_cast<long long>(depth) * 8);
    const int nWlFrame = static_cast<int>((totalCols - (wlOffset - 1)) / 2);
    const int K = nWlFrame / 8;                       // 每通道每波长每圈
    const int nTriggers = triggersOverride > 0 ? triggersOverride : 2 * K;
    if (K < 2 || nTriggers < 2) {
        std::fprintf(stderr, "K=%d triggers=%d invalid\n", K, nTriggers);
        return 2;
    }
    std::fprintf(stderr,
        "replay: %s id=%d depth=%d bits=%d K=%d triggers=%d "
        "packetsPerTrig=%d ports=%d~%d\n",
        dataPath.c_str(), id, depth, bits, K, nTriggers, packetsPerTrig,
        portBase, portBase + nCards - 1);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SOCKET socks[4];
    sockaddr_in dst[4];
    for (int c = 0; c < nCards; ++c) {
        socks[c] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int sndbuf = 32 * 1024 * 1024;
        setsockopt(socks[c], SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
        dst[c] = {};
        dst[c].sin_family = AF_INET;
        dst[c].sin_port = htons(static_cast<u_short>(portBase + c));
        inet_pton(AF_INET, "127.0.0.1", &dst[c].sin_addr);
    }

    // 每通道独立文件流，按触发顺序连续读取；
    // 超过单圈（2*K 触发）后按圈回绕，多圈回放重复同一圈数据（与真实多圈扫描一致）
    const int triggersPerRev = 2 * K;
    std::ifstream chStream[8];
    std::vector<double> col[8];
    for (int s = 0; s < 8; ++s) {
        chStream[s].open(dataPath, std::ios::binary);
        readColumn(chStream[s], depth,
                   static_cast<long long>(wlOffset - 1) + 2LL * s * K, col[s]);
    }
    std::vector<uint8_t> pkt(UDP_HEADER_BYTES + UDP_PAYLOAD_BYTES);
    const int pairBytes = bytesPerPair;
    // 补偿每触发发送开销（约 1~2ms），使实际触发率更接近目标
    const int msPerTrig = rateHz > 0 ? std::max(1, static_cast<int>(1000.0 / rateHz) - 1) : 0;

    for (int g = 0; g < nTriggers; ++g) {
        // 每卡构造本触发载荷：B/A 交织
        for (int c = 0; c < nCards; ++c) {
            const int chA = 2 * c;
            const int chB = 2 * c + 1;
            std::vector<uint8_t> payload(bytesPerTrig);
            for (int i = 0; i < depth; ++i) {
                double vB = col[chB][i];
                double vA = col[chA][i];
                if (bits == 16) {
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
                const uint16_t packetSeq =
                    static_cast<uint16_t>(static_cast<uint64_t>(g) * packetsPerTrig + p);
                const uint16_t triggerSeq = static_cast<uint16_t>(g);
                pkt[0] = static_cast<uint8_t>(packetSeq & 0xFF);
                pkt[1] = static_cast<uint8_t>((packetSeq >> 8) & 0xFF);
                pkt[2] = static_cast<uint8_t>(triggerSeq & 0xFF);
                pkt[3] = static_cast<uint8_t>((triggerSeq >> 8) & 0xFF);
                const int off = p * UDP_PAYLOAD_BYTES;
                const int len = std::min(UDP_PAYLOAD_BYTES, bytesPerTrig - off);
                std::memcpy(pkt.data() + UDP_HEADER_BYTES, payload.data() + off, len);
                // 发送失败（瞬时缓冲满）时重试，确保回放数据零丢失
                for (int attempt = 0; attempt < 200; ++attempt) {
                    const int sent = sendto(socks[c],
                        reinterpret_cast<const char*>(pkt.data()),
                        UDP_HEADER_BYTES + len, 0,
                        reinterpret_cast<sockaddr*>(&dst[c]), sizeof(dst[c]));
                    if (sent == UDP_HEADER_BYTES + len) break;
                    if (sent == SOCKET_ERROR) {
                        const int err = WSAGetLastError();
                        if (err != WSAEWOULDBLOCK && err != WSAENOBUFS) break;
                    }
                    Sleep(1);
                }
            }
        }

        // 触发前进一列：各通道流读取下一列（按圈回绕）
        for (int s = 0; s < 8; ++s) {
            const int colInRev = (g + 1) % triggersPerRev;
            readColumn(chStream[s], depth,
                       static_cast<long long>(wlOffset - 1) + 2LL * s * K + colInRev,
                       col[s]);
        }

        if (msPerTrig > 0) {
#ifdef _WIN32
            preciseSleepMs(msPerTrig);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(msPerTrig));
#endif
        }
        if ((g + 1) % 100 == 0)
            std::fprintf(stderr, "sent %d/%d triggers\n", g + 1, nTriggers);
    }

    for (int c = 0; c < nCards; ++c) closesocket(socks[c]);
    std::fprintf(stderr, "replay done: %d triggers, %d packets\n",
                 nTriggers, nTriggers * nCards * packetsPerTrig);
    return 0;
}
