// ring_recon_verify — M1 验证工具
// 按 Handoff 的逐块链路读取 .dat，输出每波长每块归一化重建图（float32 raw），
// 与 MATLAB export_ring_recon_reference.m 输出逐块对比。
#include "ring_recon.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using ringrecon::IncrementalState;
using ringrecon::PreprocessParams;
using ringrecon::ReconParams;
using ringrecon::StreamConfig;

struct DataSpec {
    int    sampDepth;
    int    wlOffset;
    int    alinesPerFrame;
    int    sysDelay1;
    int    sysDelay2;
    double radius;
    double coverageDeg;
    double fovDeg;
};

static DataSpec specFor(int id) {
    if (id == 11) return {4000, 151, 4000, 171, 184, 6.57e-3, 180.0, 180.0};
    return {4000, 301, 8000, 358, 371, 6.57e-3, 360.0, 360.0};
}

static void writeRaw(const fs::path& path, const std::vector<float>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size() * sizeof(float)));
}

static double argDouble(const std::vector<std::string>& args, const std::string& key, double def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return std::stod(args[i + 1]);
    return def;
}
static int argInt(const std::vector<std::string>& args, const std::string& key, int def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return std::stoi(args[i + 1]);
    return def;
}
static std::string argStr(const std::vector<std::string>& args, const std::string& key,
                          const std::string& def = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return args[i + 1];
    return def;
}

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string dataPath = argStr(args, "--data");
    const int id = argInt(args, "--id", 11);
    const double gridMm = argDouble(args, "--grid-mm", 0.1);
    const int block = argInt(args, "--block", 200);
    const std::string outDir = argStr(args, "--out", "ring_recon_out");
    const double theta0 = argDouble(args, "--theta0", 0.0);
    const double sosRadiusMm = argDouble(args, "--sos-radius-mm", 0.0);
    const double sos1 = argDouble(args, "--sos1", 1490.0);
    const double sos2 = argDouble(args, "--sos2", 1540.0);

    if (dataPath.empty()) {
        std::fprintf(stderr,
            "usage: ring_recon_verify --data <11.dat|14.dat> --id 11|14 "
            "[--grid-mm 0.1] [--block 200] [--out <dir>] "
            "[--theta0 0] [--sos-radius-mm 0] [--sos1 1490] [--sos2 1540]\n");
        return 2;
    }

    const DataSpec spec = specFor(id);
    StreamConfig cfg;
    cfg.sampDepth = spec.sampDepth;
    cfg.alinesPerFrame = spec.alinesPerFrame;
    cfg.alinesPerBlock = block;
    cfg.wlOffset = spec.wlOffset;
    cfg.shiftWL2 = true;
    cfg.recon.fs = 200e6;
    cfg.recon.c = 1490.0;
    cfg.recon.R = spec.radius;
    cfg.recon.fov = 36e-3;
    cfg.recon.gridSize = gridMm * 1e-3;
    cfg.recon.coverageDeg = spec.coverageDeg;
    cfg.recon.theta0Deg = theta0;
    cfg.recon.fovDeg = spec.fovDeg;
    cfg.recon.fovTheta0Deg = 0.0;
    cfg.recon.distanceWeightExponent = 1.0;
    cfg.recon.maskOutOfRange = true;
    cfg.recon.interpolation = "linear";
    if (sosRadiusMm > 0.0) {
        cfg.recon.soundSpeedRadii = {sosRadiusMm * 1e-3};
        cfg.recon.soundSpeeds = {sos1, sos2};
    }

    for (int w = 0; w < 2; ++w) {
        cfg.pre[w].systemDelay = (w == 0 ? spec.sysDelay1 : spec.sysDelay2);
        cfg.pre[w].dbrmaskExtra = (w == 1 ? spec.sysDelay2 - spec.sysDelay1 : 0);
        cfg.pre[w].maskLength = 300;
        cfg.pre[w].dbrRemove = true;
        cfg.pre[w].delayCut = true;
        cfg.pre[w].signalImpair = false;
        cfg.pre[w].imValue = (w == 0 ? 2000.0 : 400.0);
    }

    if (cfg.alinesPerBlock < 2 || cfg.alinesPerBlock % 2 != 0 ||
        cfg.alinesPerFrame % cfg.alinesPerBlock != 0) {
        std::fprintf(stderr, "invalid block size\n");
        return 2;
    }

    std::vector<float> xv, yv;
    ringrecon::makeGrid(cfg.recon.fov, cfg.recon.gridSize, xv, yv);
    const int nx = static_cast<int>(xv.size());
    const int ny = static_cast<int>(yv.size());
    const int nBlocks = cfg.alinesPerFrame / cfg.alinesPerBlock;
    const int nWlPerBlock = cfg.alinesPerBlock / 2;
    const int nWlPerFrame = cfg.alinesPerFrame / 2;
    const double dtheta = cfg.recon.coverageDeg / nWlPerFrame;
    const double spanDeg = (nWlPerBlock - 1) * dtheta;

    fs::create_directories(outDir);
    writeRaw(fs::path(outDir) / "grid_x_f32.raw", xv);
    writeRaw(fs::path(outDir) / "grid_y_f32.raw", yv);

    IncrementalState st[2];
    std::vector<double> raw, prevWL2Last, wl1, wl2, curWL2Last;
    double totalMs = 0.0;

    for (int k = 0; k < nBlocks; ++k) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!ringrecon::readRawBlock(dataPath, cfg.sampDepth, cfg.wlOffset,
                                     cfg.alinesPerBlock, k, raw)) {
            std::fprintf(stderr, "read block %d failed\n", k);
            return 2;
        }
        ringrecon::splitBlock(raw, cfg.sampDepth, nWlPerBlock, cfg.shiftWL2,
                              prevWL2Last, std::vector<double>(), wl1, wl2, curWL2Last);

        const double startDeg = cfg.recon.theta0Deg + k * nWlPerBlock * dtheta;
        for (int w = 0; w < 2; ++w) {
            const std::vector<double>& src = (w == 0 ? wl1 : wl2);
            std::vector<float> bscan = ringrecon::preprocessBlock(
                src, cfg.sampDepth, nWlPerBlock, cfg.pre[w]);
            ringrecon::dasReconAppend(bscan,
                                      static_cast<int>(bscan.size() / nWlPerBlock),
                                      nWlPerBlock, cfg.recon, startDeg, spanDeg,
                                      xv, yv, st[w]);
            std::vector<float> img = ringrecon::normalizedImage(st[w]);
            char name[32];
            std::snprintf(name, sizeof(name), "wl%d_%03d.raw", w + 1, k + 1);
            writeRaw(fs::path(outDir) / name, img);
            char accName[40], accwName[40];
            std::snprintf(accName, sizeof(accName), "wl%d_acc_%03d.raw", w + 1, k + 1);
            std::snprintf(accwName, sizeof(accwName), "wl%d_accw_%03d.raw", w + 1, k + 1);
            writeRaw(fs::path(outDir) / accName, st[w].acc);
            writeRaw(fs::path(outDir) / accwName, st[w].accW);
        }
        prevWL2Last = curWL2Last;

        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        totalMs += ms;
        std::printf("block %3d/%d: %.2f ms\n", k + 1, nBlocks, ms);
    }

    std::printf("done: nx=%d ny=%d blocks=%d total=%.1f ms avg=%.2f ms\n",
                nx, ny, nBlocks, totalMs, totalMs / nBlocks);
    return 0;
}