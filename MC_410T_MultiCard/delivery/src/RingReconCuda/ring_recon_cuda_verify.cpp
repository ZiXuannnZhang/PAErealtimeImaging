// ring_recon_cuda_verify.cpp — CUDA 版逐块验证入口
// 预处理（DBR/延时截断）沿用 CPU 版 ring_recon.cpp，重建核心走 CUDA；
// 输出与 ring_recon_verify 相同命名（wl*_xxx.raw / acc / accw），
// 可直接用 tools/compare_ring_recon.py 与 CPU/MATLAB 结果对照。
#include "../RingRecon/ring_recon.h"
#include "../RingRecon/ring_recon_cuda.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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
    const std::string outDir = argStr(args, "--out", "ring_recon_cuda_out");
    const double theta0 = argDouble(args, "--theta0", 0.0);
    const double sosRadiusMm = argDouble(args, "--sos-radius-mm", 0.0);
    const double sos1 = argDouble(args, "--sos1", 1490.0);
    const double sos2 = argDouble(args, "--sos2", 1540.0);

    if (dataPath.empty()) {
        std::fprintf(stderr,
            "usage: ring_recon_cuda_verify --data <11.dat|14.dat> --id 11|14 "
            "[--grid-mm 0.1] [--block 200] [--out <dir>] "
            "[--theta0 0] [--sos-radius-mm 0] [--sos1 1490] [--sos2 1540]\n");
        return 2;
    }

    const DataSpec spec = specFor(id);

    RingReconCudaConfig cfg;
    ring_recon_cuda_set_defaults(&cfg);
    cfg.dataNum = id;
    cfg.sampDepth = spec.sampDepth;
    cfg.reconDepth = spec.sampDepth;
    cfg.alinesPerFrame = spec.alinesPerFrame;
    cfg.alinesPerBlock = block;
    cfg.radius = spec.radius;
    cfg.fovDeg = spec.fovDeg;
    cfg.gridSize = gridMm * 1e-3;
    cfg.sysDelay[0] = spec.sysDelay1;
    cfg.sysDelay[1] = spec.sysDelay2;
    if (sosRadiusMm > 0.0) {
        cfg.soundSpeedRadiiCount = 1;
        cfg.soundSpeedRadii[0] = sosRadiusMm * 1e-3;
        cfg.soundSpeedsCount = 2;
        cfg.soundSpeeds[0] = sos1;
        cfg.soundSpeeds[1] = sos2;
    }

    if (cfg.alinesPerBlock < 2 || cfg.alinesPerBlock % 2 != 0 ||
        cfg.alinesPerFrame % cfg.alinesPerBlock != 0) {
        std::fprintf(stderr, "invalid block size\n");
        return 2;
    }

    const int nBlocks = cfg.alinesPerFrame / cfg.alinesPerBlock;
    const int nWlPerBlock = cfg.alinesPerBlock / 2;
    const int nWlPerFrame = cfg.alinesPerFrame / 2;
    const double dtheta = spec.coverageDeg / nWlPerFrame;
    const double spanDeg = (nWlPerBlock - 1) * dtheta;

    void* h[2] = {nullptr, nullptr};
    if (ring_recon_cuda_create(&cfg, &h[0]) != 0 ||
        ring_recon_cuda_create(&cfg, &h[1]) != 0) {
        std::fprintf(stderr, "create failed: %s\n", ring_recon_cuda_last_error());
        return 2;
    }
    int nx = 0, ny = 0;
    ring_recon_cuda_grid_size(h[0], &nx, &ny);

    fs::create_directories(outDir);
    std::vector<float> xv, yv;
    ringrecon::makeGrid(cfg.fov, cfg.gridSize, xv, yv);
    writeRaw(fs::path(outDir) / "grid_x_f32.raw", xv);
    writeRaw(fs::path(outDir) / "grid_y_f32.raw", yv);

    std::vector<double> raw, prevWL2Last, wl1, wl2, curWL2Last;
    double totalMs = 0.0;
    for (int k = 0; k < nBlocks; ++k) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!ringrecon::readRawBlock(dataPath, cfg.sampDepth, spec.wlOffset,
                                     cfg.alinesPerBlock, k, raw)) {
            std::fprintf(stderr, "read block %d failed\n", k);
            return 2;
        }
        ringrecon::splitBlock(raw, cfg.sampDepth, nWlPerBlock, cfg.shiftWL2 != 0,
                              prevWL2Last, std::vector<double>(), wl1, wl2, curWL2Last);

        const double startDeg = theta0 + k * nWlPerBlock * dtheta;
        for (int w = 0; w < 2; ++w) {
            const std::vector<double>& src = (w == 0 ? wl1 : wl2);
            ringrecon::PreprocessParams pp;
            pp.systemDelay = (w == 0 ? spec.sysDelay1 : spec.sysDelay2);
            pp.dbrmaskExtra = (w == 1 ? spec.sysDelay2 - spec.sysDelay1 : 0);
            pp.maskLength = cfg.maskLength;
            pp.dbrRemove = cfg.dbrSigRemove != 0;
            pp.delayCut = cfg.delayCut != 0;
            pp.signalImpair = cfg.singalImpair != 0;
            pp.imValue = cfg.imValue[w];

            std::vector<float> bscan = ringrecon::preprocessBlock(
                src, cfg.sampDepth, nWlPerBlock, pp);
            const int nt = static_cast<int>(bscan.size() / nWlPerBlock);
            if (ring_recon_cuda_append(h[w], bscan.data(), nt, nWlPerBlock,
                                       startDeg, spanDeg) != 0) {
                std::fprintf(stderr, "cuda append failed: %s\n",
                             ring_recon_cuda_last_error());
                return 2;
            }

            std::vector<float> acc(static_cast<size_t>(nx) * ny);
            std::vector<float> accW(static_cast<size_t>(nx) * ny);
            ring_recon_cuda_get_state(h[w], acc.data(), accW.data(), nullptr);
            std::vector<float> img(acc.size());
            for (size_t i = 0; i < img.size(); ++i) {
                img[i] = acc[i] / std::max(accW[i], 1e-12f);
            }

            char name[40];
            std::snprintf(name, sizeof(name), "wl%d_%03d.raw", w + 1, k + 1);
            writeRaw(fs::path(outDir) / name, img);
            std::snprintf(name, sizeof(name), "wl%d_acc_%03d.raw", w + 1, k + 1);
            writeRaw(fs::path(outDir) / name, acc);
            std::snprintf(name, sizeof(name), "wl%d_accw_%03d.raw", w + 1, k + 1);
            writeRaw(fs::path(outDir) / name, accW);
        }
        prevWL2Last = curWL2Last;

        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        totalMs += ms;
        std::printf("block %3d/%d: %.2f ms\n", k + 1, nBlocks, ms);
    }

    // 方案A 快照自检：snapshot(step=1, dn=nx) 应等于 acc/accW 归一化；
    // reset 后 get_state 应为全零。
    for (int w = 0; w < 2; ++w) {
        std::vector<float> acc(static_cast<size_t>(nx) * ny);
        std::vector<float> accW(static_cast<size_t>(nx) * ny);
        std::vector<float> snap(static_cast<size_t>(nx) * ny);
        if (ring_recon_cuda_get_state(h[w], acc.data(), accW.data(), nullptr) != 0 ||
            ring_recon_cuda_snapshot(h[w], nx, 1, snap.data()) != 0) {
            std::fprintf(stderr, "snapshot/get_state failed: %s\n",
                         ring_recon_cuda_last_error());
            return 2;
        }
        double worst = 0.0;
        for (int i = 0; i < nx * ny; ++i) {
            const float exp = acc[i] / std::max(accW[i], 1e-12f);
            worst = std::max(worst, static_cast<double>(std::fabs(snap[i] - exp)));
        }
        std::printf("wl%d snapshot vs get_state worst diff = %.3e\n", w + 1, worst);
        if (worst > 1e-4) {
            std::fprintf(stderr, "snapshot mismatch\n");
            return 2;
        }
        if (ring_recon_cuda_reset(h[w]) != 0) {
            std::fprintf(stderr, "reset failed: %s\n", ring_recon_cuda_last_error());
            return 2;
        }
        std::vector<float> z(static_cast<size_t>(nx) * ny);
        std::vector<float> zw(static_cast<size_t>(nx) * ny);
        ring_recon_cuda_get_state(h[w], z.data(), zw.data(), nullptr);
        bool zero = true;
        for (int i = 0; i < nx * ny; ++i) {
            if (z[i] != 0.0f || zw[i] != 0.0f) { zero = false; break; }
        }
        if (!zero) {
            std::fprintf(stderr, "reset not zero\n");
            return 2;
        }
        std::printf("wl%d reset ok\n", w + 1);
    }

    ring_recon_cuda_destroy(h[0]);
    ring_recon_cuda_destroy(h[1]);

    std::printf("done: nx=%d ny=%d blocks=%d total=%.1f ms avg=%.2f ms\n",
                nx, ny, nBlocks, totalMs, totalMs / nBlocks);
    return 0;
}
