// ring_svc_selftest — M3 环形链路自检（8 通道扇区模型）
// 用 14.dat 合成 8 通道数据：单探测器 A-line 按角度扇区连续切分给各通道
// （通道 s 取文件内角度落在其扇区的连续 K 根 A-line），构造 trigger-major
// 原始块并附带每根 A-line 的通道号与角度，经共享内存+ZMQ 驱动 ImagingSvc
// 环形重建，逐块保存 wl1/wl2 帧供对照。多通道模拟要求 360° 数据集。
#include "ImagingSharedMemory.h"
#include "RingBlockAssembler.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSharedMemory>
#include <QThread>

#include <zmq.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr int kPort = 5555;
constexpr int kTimeoutMs = 20000;

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
double argDouble(const std::vector<std::string>& args, const std::string& key, double def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return std::stod(args[i + 1]);
    return def;
}

bool sendJson(zmq::socket_t& sock, const QJsonObject& obj) {
    const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    zmq::message_t msg(static_cast<size_t>(data.size()));
    std::memcpy(msg.data(), data.constData(), static_cast<size_t>(data.size()));
    try {
        sock.send(msg, zmq::send_flags::dontwait);
        return true;
    } catch (...) {
        return false;
    }
}

bool readColumn(const std::string& path, int sampDepth, int col0,
                std::vector<double>& col) {
    col.resize(sampDepth);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(col0) * sampDepth * 8);
    f.read(reinterpret_cast<char*>(col.data()),
           static_cast<std::streamsize>(sampDepth * 8));
    return f.good() || f.gcount() == static_cast<std::streamsize>(sampDepth * 8);
}

bool saveRaw(const std::string& path, const std::vector<float>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size() * sizeof(float)));
    return f.good();
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const std::vector<std::string> args(argv + 1, argv + argc);

    const std::string dataPath = argStr(args, "--data");
    const int id = argInt(args, "--id", 11);
    const double gridMm = argDouble(args, "--grid-mm", 0.1);
    const int perChBlock = argInt(args, "--block", 200);      // 每通道每块 A-line 数
    const int chMask = argInt(args, "--channels", 0xFF);      // 勾选通道位图
    const double sectorStart = argDouble(args, "--sector-start", 180.0);
    const std::string outDir = argStr(args, "--out", "ring_svc_out");
    const std::string svcPath = argStr(args, "--svc");
    const std::string sosRadiiMm = argStr(args, "--sos-radii-mm");   // 空=单一声速
    const std::string sosSpeeds = argStr(args, "--sos", "1490,1540");
    const double timeoutResetSec = argDouble(args, "--timeout-reset", 0.0);
    const bool noLaunch = argInt(args, "--no-launch", 0) != 0;      // svc 由外部启动（沙箱环境用）
    const std::string radiusPerChMm = argStr(args, "--radius-per-ch");  // 空=统一半径；8 个毫米值逗号分隔
    const bool splice = argInt(args, "--splice", 0) != 0;          // 1=拼接模式（需配准模式）
    const double spliceBlendDeg = argDouble(args, "--splice-blend", 0.0);  // 拼接羽化宽度（°）
    const bool cfGate = argInt(args, "--cf-gate", 0) != 0;               // 1=CF 门控（与拼接互斥）
    const double cfGain = argDouble(args, "--cf-gain", 0.5);             // CF 增益指数
    const int nonlinearMode = argInt(args, "--nonlinear", 0);            // 0=关闭；1=pCF；2=DMAS

    if (dataPath.empty() || svcPath.empty()) {
        std::fprintf(stderr,
            "usage: ring_svc_selftest --data <11.dat> --svc <ImagingSvc.exe> "
            "[--id 11] [--grid-mm 0.1] [--block 200] [--channels 255] "
            "[--sector-start 180] [--out <dir>] "
            "[--radius-per-ch 6.57,6.55,...] [--splice 1] [--splice-blend 1.5] "
            "[--cf-gate 1] [--cf-gain 0.5] [--nonlinear 0|1|2] "
            "[--sos-radii-mm 3] [--sos 1490,1540]\n");
        return 2;
    }

    if (splice && radiusPerChMm.empty()) {
        std::fprintf(stderr, "--splice requires --radius-per-ch (multiRadius mode)\n");
        return 2;
    }
    if (spliceBlendDeg < 0.0) {
        std::fprintf(stderr, "--splice-blend must be >= 0\n");
        return 2;
    }
    if (splice && cfGate) {
        std::fprintf(stderr, "--cf-gate is mutually exclusive with --splice\n");
        return 2;
    }
    if (cfGain < 0.0 || cfGain > 1.0) {
        std::fprintf(stderr, "--cf-gain must be within [0,1]\n");
        return 2;
    }
    if (nonlinearMode < 0 || nonlinearMode > 2) {
        std::fprintf(stderr, "--nonlinear must be 0 (off), 1 (pCF) or 2 (DMAS)\n");
        return 2;
    }
    if (splice && nonlinearMode != 0) {
        std::fprintf(stderr, "--nonlinear is mutually exclusive with --splice\n");
        return 2;
    }

    int channels[8] = {0};
    int cnt = 0;
    for (int c = 0; c < 8; ++c)
        if (chMask & (1 << c)) { channels[c] = 1; ++cnt; }
    if (cnt == 0) { std::fprintf(stderr, "no channel selected\n"); return 2; }
    if (perChBlock < 2 || perChBlock % 2 != 0) {
        std::fprintf(stderr, "block must be positive even\n");
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::fprintf(stderr, "create out dir failed: %s\n", ec.message().c_str());
        return 2;
    }

    const int sampDepth = 4000;
    const int wlOffset = (id == 11) ? 151 : 301;
    const int alinesPerFrame = (id == 11) ? 4000 : 8000;      // 双波长合计
    const int sysDelay1 = (id == 11) ? 171 : 358;
    const int sysDelay2 = (id == 11) ? 184 : 371;
    const double radius = 6.57e-3;
    const double coverageDeg = (id == 11) ? 180.0 : 360.0;
    const double fovDeg = coverageDeg;

    const int nWlFrame = alinesPerFrame / 2;                  // 单波长总 A-line 数
    const int K = nWlFrame / cnt;                              // 每通道每波长每圈 A-line 数
    if (K < 2) { std::fprintf(stderr, "K too small: %d\n", K); return 2; }
    if (cnt > 1 && coverageDeg < 359.9999) {
        std::fprintf(stderr,
            "multi-channel simulation requires a 360-degree dataset (use --id 14)\n");
        return 2;
    }
    if (K % (perChBlock / 2) != 0) {
        std::fprintf(stderr,
            "K=%d not divisible by perChBlock/2=%d; choose --block so that K*2 %% block == 0\n",
            K, perChBlock / 2);
        return 2;
    }
    const int nBlocks = K / (perChBlock / 2);                  // 每波长每通道块数
    const int alines = cnt * perChBlock;
    const int blockSize = sampDepth * alines;
    const int nx = static_cast<int>(std::ceil(0.036 / (gridMm * 1e-3)));
    const int frameSize = nx * nx;
    const double sectorWidth = (coverageDeg >= 359.9999) ? 360.0 / cnt : coverageDeg;
    const double step = sectorWidth / K;

    // ZMQ + 共享内存
    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::pair);
    try {
        sock.bind("tcp://127.0.0.1:" + std::to_string(kPort));
    } catch (const zmq::error_t& e) {
        std::fprintf(stderr, "ZMQ bind failed: %s\n", e.what());
        return 2;
    }

    QSharedMemory shm("MC_410T_RingShm");
    if (!shm.create(static_cast<int>(
            ringImagingShmTotalSizeV2(blockSize, frameSize, alines, frameSize)))) {
        if (shm.error() == QSharedMemory::AlreadyExists) shm.attach();
        else { std::fprintf(stderr, "ring shm create failed\n"); return 2; }
    }
    shm.lock();
    std::memset(shm.data(), 0,
                ringImagingShmTotalSizeV2(blockSize, frameSize, alines, frameSize));
    auto* hdr = static_cast<RingImagingShmHeader*>(shm.data());
    hdr->magic = 0x52494E47;
    hdr->version = 2;   // 方案A：显示区=全分辨率区，display 字段与 v2 强校验一致
    hdr->block_size = static_cast<uint32_t>(blockSize);
    hdr->frame_size = static_cast<uint32_t>(frameSize);
    hdr->alines = static_cast<uint32_t>(alines);
    hdr->nx = static_cast<uint16_t>(nx);
    hdr->ny = static_cast<uint16_t>(nx);
    hdr->display_nx = static_cast<uint16_t>(nx);
    hdr->display_ny = static_cast<uint16_t>(nx);
    hdr->display_frame_size = static_cast<uint32_t>(frameSize);
    hdr->snapshot_step = 1;
    hdr->flags = 0;
    shm.unlock();

    QProcess svc;
    if (!noLaunch) {
        svc.setProcessChannelMode(QProcess::ForwardedChannels);
        svc.start(QString::fromStdString(svcPath), QStringList());
        if (!svc.waitForStarted(10000)) { std::fprintf(stderr, "ImagingSvc start failed\n"); return 2; }
        QThread::msleep(500);
    } else {
        // 外部启动的 svc 早已 connect；等待其 ZMQ 重连到本端 bind 完成，
        // 否则连接建立前的 configure 会被丢弃导致 svc 不响应。
        QThread::msleep(1500);
    }

    QJsonObject ring;
    ring["sampDepth"] = sampDepth;
    ring["reconDepth"] = sampDepth;
    ring["alinesPerFrame"] = alinesPerFrame;
    ring["alinesPerBlock"] = perChBlock;
    ring["shiftWL2"] = 1;
    ring["daqHz"] = 200e6;
    ring["radius"] = radius;
    // 多扫描半径配准：--radius-per-ch "r0,r1,..."（毫米）时下发逐通道半径
    {
        std::vector<double> rpc;
        size_t pos = 0;
        while (pos <= radiusPerChMm.size()) {
            const size_t comma = radiusPerChMm.find(',', pos);
            const std::string tok = radiusPerChMm.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!tok.empty()) rpc.push_back(std::stod(tok) * 1e-3);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (!rpc.empty()) {
            ring["multiRadius"] = 1;
            QJsonArray rpcArr;
            for (size_t i = 0; i < 8; ++i)
                rpcArr.append(i < rpc.size() ? rpc[i] : rpc.back());
            ring["radiusPerChannel"] = rpcArr;
        }
    }
    ring["spliceMode"] = splice ? 1 : 0;
    ring["spliceBlendDeg"] = splice ? spliceBlendDeg : 0.0;
    ring["coherenceGate"] = cfGate ? 1 : 0;
    ring["coherenceGain"] = cfGain;
    ring["coherenceEpsilon"] = 1e-12;
    ring["nonlinearMode"] = nonlinearMode;
    QJsonArray speeds;
    {
        size_t pos = 0;
        while (pos <= sosSpeeds.size()) {
            const size_t comma = sosSpeeds.find(',', pos);
            const std::string tok = sosSpeeds.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!tok.empty()) speeds.append(std::stod(tok));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    ring["soundSpeeds"] = speeds;
    QJsonArray radii;
    {
        size_t pos = 0;
        while (pos <= sosRadiiMm.size()) {
            const size_t comma = sosRadiiMm.find(',', pos);
            const std::string tok = sosRadiiMm.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!tok.empty()) radii.append(std::stod(tok) * 1e-3);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    ring["soundSpeedRadii"] = radii;
    ring["soundSpeedRadiiCount"] = static_cast<int>(radii.size());
    ring["fov"] = 36e-3;
    ring["gridSize"] = gridMm * 1e-3;
    ring["fovDeg"] = fovDeg;
    ring["fovTheta0Deg"] = 0.0;
    ring["apodType"] = 0;
    ring["distanceWeightExponent"] = 1.0;
    ring["interpolation"] = 0;
    ring["minDistance"] = 0.0;
    ring["maskOutOfRange"] = 1;
    ring["dbrSigRemove"] = 1;
    ring["maskLength"] = 300;
    ring["delayCut"] = 1;
    ring["singalImpair"] = 0;
    QJsonArray imv; imv.append(2000.0); imv.append(400.0);
    ring["imValue"] = imv;
    QJsonArray sd; sd.append(sysDelay1); sd.append(sysDelay2);
    ring["sysDelay"] = sd;
    QJsonArray chArr;
    for (int c = 0; c < 8; ++c) chArr.append(channels[c]);
    ring["enabledChannels"] = chArr;
    ring["enabledChannelCount"] = cnt;
    ring["alinesPerChannelPerBlock"] = perChBlock;
    ring["alinesPerChannelPerFrame"] = K;
    ring["sectorStartDeg"] = sectorStart;
    ring["sectorCcw"] = 1;
    ring["triggerWlOdd"] = 1;

    QJsonObject params;
    params["imagingMode"] = "ring";
    params["ring"] = ring;
    sendJson(sock, {{"cmd", "configure"}, {"params", params}});
    QThread::msleep(50);
    sendJson(sock, {{"cmd", "start"}});
    QThread::msleep(200);

    // 阶段B链路：用 RingBlockAssembler 按“每通道每触发”喂入（模拟真实采集
    // UDP 到达顺序），组满 perChBlock 根后回调提交环形块驱动重建。
    std::vector<int> selIdx(8, -1);
    int si = 0;
    for (int c = 0; c < 8; ++c) if (channels[c]) selIdx[c] = si++;

    double totalMs = 0.0;
    std::vector<float> lastWl1, lastWl2;   // 逐块对比缓存
    RingBlockAssembler assembler;
    assembler.setBlockCallback([&](std::vector<float> &&raw, std::vector<float> &&angles,
                                   std::vector<uint8_t> &&chIds, int blockSeq) {
        const auto t0 = std::chrono::steady_clock::now();
        shm.lock();
        auto* h = static_cast<RingImagingShmHeader*>(shm.data());
        std::memcpy(reinterpret_cast<float*>(h + 1), raw.data(),
                    static_cast<size_t>(blockSize) * sizeof(float));
        std::memcpy(reinterpret_cast<uint8_t*>(h + 1) + ringAnglesOffset(blockSize),
                    angles.data(), static_cast<size_t>(alines) * sizeof(float));
        std::memcpy(reinterpret_cast<uint8_t*>(h + 1) + ringChannelsOffset(blockSize, alines),
                    chIds.data(), static_cast<size_t>(alines));
        h->block_seq = static_cast<uint32_t>(blockSeq);
        h->block_ready = 1;
        shm.unlock();
        sendJson(sock, {{"cmd", "ring_block_ready"}, {"seq", blockSeq}});

        bool got = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            zmq::message_t msg;
            while (sock.recv(msg, zmq::recv_flags::dontwait)) {
                const QByteArray data(static_cast<const char*>(msg.data()), static_cast<int>(msg.size()));
                const QJsonObject obj = QJsonDocument::fromJson(data).object();
                if (obj["cmd"].toString() == "ring_snapshot_ready") { got = true; break; }
            }
            if (got) break;
            QThread::msleep(5);
        }
        if (!got) {
            std::fprintf(stderr, "timeout at block %d\n", blockSeq);
            std::exit(2);
        }

        std::vector<float> wl1(frameSize), wl2(frameSize);
        shm.lock();
        h = static_cast<RingImagingShmHeader*>(shm.data());
        auto* fb = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(h + 1) +
                                           ringFramesOffset(blockSize, alines));
        std::memcpy(wl1.data(), fb, static_cast<size_t>(frameSize) * sizeof(float));
        std::memcpy(wl2.data(), fb + frameSize, static_cast<size_t>(frameSize) * sizeof(float));
        shm.unlock();

        if (blockSeq > 0) {
            const size_t n = static_cast<size_t>(frameSize);
            double d1 = 0.0, d2 = 0.0;
            for (size_t i = 0; i < n; ++i) {
                d1 = std::max(d1, static_cast<double>(std::fabs(wl1[i] - lastWl1[i])));
                d2 = std::max(d2, static_cast<double>(std::fabs(wl2[i] - lastWl2[i])));
            }
            std::printf("  diff vs prev: wl1=%.6g wl2=%.6g\n", d1, d2);
        }
        lastWl1 = wl1;
        lastWl2 = wl2;

        char nm[64];
        std::snprintf(nm, sizeof(nm), "svc_wl1_%03d.raw", blockSeq + 1);
        if (!saveRaw(outDir + "/" + nm, wl1)) {
            std::fprintf(stderr, "write %s failed\n", nm);
            std::exit(2);
        }
        std::snprintf(nm, sizeof(nm), "svc_wl2_%03d.raw", blockSeq + 1);
        if (!saveRaw(outDir + "/" + nm, wl2)) {
            std::fprintf(stderr, "write %s failed\n", nm);
            std::exit(2);
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        totalMs += ms;
        std::printf("block %3d/%d: %.2f ms\n", blockSeq + 1, nBlocks, ms);
    });
    assembler.configure(channels, perChBlock, sampDepth, sectorStart,
                        sectorWidth, step, K, 1, timeoutResetSec);
    assembler.setTimeoutCallback([&]() {
        // 模拟接收端：触发级超时判定新一圈，通知 svc 清空重建累积
        sendJson(sock, {{"cmd", "ring_reset"}});
        std::fprintf(stderr, "[selftest] timeout reset fired\n");
    });

    const int nRounds = 2;   // 复现“第 2 圈起 wl2 最后一帧不刷新”的跨圈行为
    const int nBlocksTotal = nBlocks * nRounds;
    int resetBase = 0;       // 超时复位后新一圈的数据列基准（模拟重新开始采集）
    for (int b = 0; b < nBlocksTotal; ++b) {
        // 圈中停机模拟：在第 1 圈一半处暂停，验证超时后新触发判定为新一圈
        if (timeoutResetSec > 0.0 && b == nBlocks / 2) {
            resetBase = b;   // 新一圈从该块起，数据列回到 0 基准
            std::fprintf(stderr, "[selftest] pausing %.0f ms (simulate mid-round stop)\n",
                         timeoutResetSec * 1000.0 + 500.0);
            QThread::msleep(static_cast<unsigned long>(timeoutResetSec * 1000.0) + 500);
        }
        for (int t = 0; t < perChBlock; ++t) {
            const int kInBlock = t / 2;
            const int kGlobal = (b - resetBase) * (perChBlock / 2) + kInBlock;
            const int kRound = kGlobal % K;   // 多圈复用同一组列，角度按每圈 K 回绕
            for (int c = 0; c < 8; ++c) {
                if (!channels[c]) continue;
                const int s = selIdx[c];
                const int wlIdx = s * K + kRound;            // 每波长在全帧内行号
                const int col0 = wlOffset - 1 + 2 * wlIdx + (t % 2);
                std::vector<double> col;
                if (!readColumn(dataPath, sampDepth, col0, col)) {
                    std::fprintf(stderr, "read col %d failed\n", col0);
                    return 2;
                }
                std::vector<float> line(sampDepth);
                for (int r = 0; r < sampDepth; ++r)
                    line[r] = static_cast<float>(col[r]);
                assembler.pushChannelLine(c, static_cast<uint16_t>(b * perChBlock + t),
                                          line.data(), sampDepth);
            }
        }
    }

    sendJson(sock, {{"cmd", "stop"}});
    QThread::msleep(100);
    if (!noLaunch) {
        svc.terminate();
        if (!svc.waitForFinished(3000)) svc.kill();
    }

    std::printf("done: channels=%d K=%d rounds=%d blocks=%d total=%.1f ms avg=%.2f ms\n",
                cnt, K, nRounds, nBlocksTotal, totalMs, totalMs / nBlocksTotal);
    return 0;
}
