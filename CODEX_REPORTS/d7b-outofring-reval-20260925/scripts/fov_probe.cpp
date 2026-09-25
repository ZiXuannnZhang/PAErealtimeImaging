// fov_probe.cpp —— FOV/成像口径决策依据
//
// 问题：把有效 FOV 收到探测环内（ρ ≤ R）到底是收益还是损失？
//   - D7 合成：UBP 全局峰值跑到 r≈16-19mm 域外结构，环内真目标被淹没 ⇒ 疑似掩膜可解
//   - D6 实测：峰值在 |r|=9.55mm（环外）⇒ 疑似掩膜会切掉真结构
// 两者冲突，必须实测。本工具只测不改生产代码。
//
// 编译：g++ -std=c++17 -O2 -I<paths> -o fov_probe fov_probe.cpp ring_recon.cpp RingPhantomForward.cpp
#include "ring_recon.h"
#include "RingPhantomForward.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static const double FS = 250e6, C_SOUND = 1490.0, R_RING = 6.57e-3, FOV = 36e-3;
static const int SAMP_DEPTH = 4000, REC_LEN = 10000, N_CH = 8;
static const double SECTOR_START_DEG = 180.0, SECTOR_WIDTH_DEG = 45.0, STEP_DEG = 0.09;
static const int SYS_DELAY = 358, MASK_LEN = 300;

static float f16(uint16_t u) {
    const int s = (u >> 15) & 1, e = (u >> 10) & 31, m = u & 1023;
    if (e == 0) return (float)((s ? -1 : 1) * std::ldexp((double)m / 1024.0, -14));
    if (e == 31) return (float)((s ? -1 : 1) * INFINITY);
    return (float)((s ? -1 : 1) * std::ldexp(1.0 + m / 1024.0, e - 15));
}
static const char* CHAN[N_CH] = {
    "Card1_ChA","Card1_ChB","Card2_ChA","Card2_ChB",
    "Card3_ChA","Card3_ChB","Card4_ChA","Card4_ChB"};

struct Img { std::vector<float> v; int nx, ny; double x0, y0, dx, dy; };

static Img toRowMajor(const ringrecon::IncrementalState& st,
                      const std::vector<float>& xv, const std::vector<float>& yv) {
    Img r; r.nx = (int)xv.size(); r.ny = (int)yv.size();
    r.x0 = xv.front(); r.y0 = yv.front();
    r.dx = (r.nx > 1) ? xv[1] - xv[0] : 0.0;
    r.dy = (r.ny > 1) ? yv[1] - yv[0] : 0.0;
    const std::vector<float> n = ringrecon::normalizedImage(st);
    r.v.assign((size_t)r.nx * r.ny, 0.0f);
    for (int ix = 0; ix < r.nx; ++ix)
        for (int iy = 0; iy < r.ny; ++iy)
            r.v[(size_t)iy * r.nx + ix] = n[(size_t)ix * r.ny + iy];
    return r;
}

static double px(const Img& r, int ix, int iy) { return r.x0 + ix * r.dx; }
static double py(const Img& r, int ix, int iy) { return r.y0 + iy * r.dy; }

// 环内/环外能量与峰值剖分
static void reportRadial(const char* tag, const Img& r) {
    double eIn = 0, eOut = 0, e2In = 0, e2Out = 0;
    long nIn = 0, nOut = 0;
    double pkIn = -1, pkOut = -1, pkInX = 0, pkInY = 0, pkOutX = 0, pkOutY = 0;
    const int BINS = 12; const double rmax = 0.018;
    std::vector<double> eBin(BINS, 0.0);
    for (int iy = 0; iy < r.ny; ++iy)
        for (int ix = 0; ix < r.nx; ++ix) {
            const double x = px(r, ix, iy), y = py(r, ix, iy);
            const double rho = std::sqrt(x * x + y * y);
            const double a = std::fabs((double)r.v[(size_t)iy * r.nx + ix]);
            const int b = (int)(rho / rmax * BINS);
            if (b >= 0 && b < BINS) eBin[(size_t)b] += a;
            if (rho <= R_RING) {
                eIn += a; e2In += a * a; ++nIn;
                if (a > pkIn) { pkIn = a; pkInX = x; pkInY = y; }
            } else {
                eOut += a; e2Out += a * a; ++nOut;
                if (a > pkOut) { pkOut = a; pkOutX = x; pkOutY = y; }
            }
        }
    const double tot = eIn + eOut;
    std::printf("\n【%s】径向能量剖分（R = %.3f mm）\n", tag, R_RING * 1e3);
    std::printf("  环内 ρ≤R : 能量 %.4g (%.2f%%)  像素 %ld   峰值 %.4g @ (%.3f, %.3f) mm  ρ=%.3f mm\n",
                eIn, tot > 0 ? 100.0 * eIn / tot : 0, nIn, pkIn,
                pkInX * 1e3, pkInY * 1e3, std::sqrt(pkInX * pkInX + pkInY * pkInY) * 1e3);
    std::printf("  环外 ρ>R : 能量 %.4g (%.2f%%)  像素 %ld   峰值 %.4g @ (%.3f, %.3f) mm  ρ=%.3f mm\n",
                eOut, tot > 0 ? 100.0 * eOut / tot : 0, nOut, pkOut,
                pkOutX * 1e3, pkOutY * 1e3, std::sqrt(pkOutX * pkOutX + pkOutY * pkOutY) * 1e3);
    std::printf("  环内/环外 峰值比 = %.4g\n", pkOut > 0 ? pkIn / pkOut : 0);
    std::printf("  径向能量（bin 宽 %.2f mm）: ", rmax / BINS * 1e3);
    double eTot = 0; for (double e : eBin) eTot += e;
    for (int b = 0; b < BINS; ++b)
        std::printf("%.1f%% ", eTot > 0 ? 100.0 * eBin[(size_t)b] / eTot : 0.0);
    std::printf("\n");
}

int main(int argc, char** argv) {
    const std::string root = (argc > 1) ? argv[1] : "testdata/01";
    const double gridSize = (argc > 2) ? std::atof(argv[2]) : 0.1e-3;

    std::printf("==================================================================\n");
    std::printf("FOV/成像口径决策依据：真信号在环内还是环外？\n");
    std::printf("R=%.3f mm  FOV=%.1f mm  gridSize=%.3f mm\n",
                R_RING * 1e3, FOV * 1e3, gridSize * 1e3);
    std::printf("==================================================================\n");

    // ---------- 真实数据 ----------
    struct ChanData { std::vector<std::vector<float>> wl[2]; };
    std::vector<ChanData> cd(N_CH);
    for (int c = 0; c < N_CH; ++c)
        for (int f = 0; f < 8; ++f) {
            char name[256];
            std::snprintf(name, sizeof name, "%s/%s_test_%03d.dat", root.c_str(), CHAN[c], f);
            std::ifstream in(name, std::ios::binary);
            if (!in) continue;
            std::vector<uint16_t> raw((size_t)REC_LEN * 100);
            in.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)(raw.size() * 2));
            const int recs = (int)((size_t)in.gcount() / 2 / REC_LEN);
            for (int r = 0; r < recs; ++r) {
                const int g = f * recs + r;
                const int wl = (g % 2 == 0) ? 0 : 1;
                std::vector<float> line(SAMP_DEPTH);
                for (int i = 0; i < SAMP_DEPTH; ++i) {
                    float v = f16(raw[(size_t)r * REC_LEN + (SYS_DELAY - 1) + i]);
                    if (i < MASK_LEN) v = 0.0f;
                    line[i] = v;
                }
                cd[(size_t)c].wl[(size_t)wl].push_back(std::move(line));
            }
        }

    std::vector<float> xv, yv;
    ringrecon::makeGrid(FOV, gridSize, xv, yv);

    for (int mode = 0; mode < 2; ++mode) {
        ringrecon::ReconParams rp;
        rp.fs = FS; rp.c = C_SOUND; rp.R = R_RING; rp.fov = FOV; rp.gridSize = gridSize;
        rp.distanceWeightExponent = 1.0; rp.minDistance = 0.0;
        rp.maskOutOfRange = true; rp.interpolation = "linear";
        rp.inversion = ringrecon_inv::InversionMode::Das;
        ringrecon::IncrementalState st;
        for (int c = 0; c < N_CH; ++c) {
            const auto& lines = cd[(size_t)c].wl[0];          // wl1
            const int nd = (int)lines.size();
            if (nd < 2) continue;
            std::vector<float> bscan((size_t)nd * SAMP_DEPTH);
            for (int j = 0; j < nd; ++j)
                std::memcpy(bscan.data() + (size_t)j * SAMP_DEPTH,
                            lines[(size_t)j].data(), SAMP_DEPTH * sizeof(float));
            const double start = SECTOR_START_DEG + c * SECTOR_WIDTH_DEG;
            if (mode == 0) {
                rp.fovDeg = 360.0; rp.fovTheta0Deg = 0.0;
            } else {
                rp.fovDeg = SECTOR_WIDTH_DEG; rp.fovTheta0Deg = start;
            }
            ringrecon::dasReconAppend(bscan, SAMP_DEPTH, nd, rp,
                                      start, STEP_DEG * (nd - 1), xv, yv, st);
        }
        reportRadial(mode == 0 ? "实测 DAS 全局" : "实测 DAS 拼接",
                     toRowMajor(st, xv, yv));
    }

    // ---------- 合成：环内点目标（D7 场景 P）----------
    {
        std::vector<double> th, rad;
        const int nd = 36;
        th.resize(nd); rad.resize(nd);
        for (int j = 0; j < nd; ++j) {
            th[(size_t)j] = (10.0 * j) * 3.14159265358979323846 / 180.0;
            rad[(size_t)j] = R_RING;
        }
        ringphantom::Source a, b;
        a.x = 3.0e-3; a.y = 1.0e-3; a.amplitude = 1.0;
        b.x = -5.0e-3; b.y = 4.0e-3; b.amplitude = 0.8;
        ringphantom::Geometry g; g.fs = FS; g.c = C_SOUND; g.sampDepth = SAMP_DEPTH;
        ringphantom::ForwardParams fp; fp.pulse = ringphantom::PulseShape::Delta;
        const auto bscan = ringphantom::simulate({a, b}, th, rad, g, fp);

        for (int inv = 0; inv < 2; ++inv) {
            ringrecon::ReconParams rp;
            rp.fs = FS; rp.c = C_SOUND; rp.R = R_RING; rp.fov = FOV; rp.gridSize = gridSize;
            rp.distanceWeightExponent = 1.0; rp.minDistance = 0.0;
            rp.maskOutOfRange = true; rp.interpolation = "linear";
            rp.fovDeg = 360.0;
            rp.inversion = inv ? ringrecon_inv::InversionMode::Ubp
                               : ringrecon_inv::InversionMode::Das;
            ringrecon::IncrementalState st;
            ringrecon::dasReconAppend(bscan, SAMP_DEPTH, nd, rp, 0.0, 350.0, xv, yv, st);
            reportRadial(inv ? "合成环内目标 UBP" : "合成环内目标 DAS",
                         toRowMajor(st, xv, yv));
        }
    }
    std::printf("\n（不改生产代码；掩膜与否的取舍由规划侧据上表决定）\n");
    return 0;
}
