// d7b_outofring.cpp —— D7 重跑：体模目标放到环外真实工作区
//
// 背景修正（用户提供）：
//   * 本项目是**内窥成像**，实际声源都在探测环**外**；
//   * 环内出现的是**对侧信号穿过整个环到达探头**造成的反投影伪影，不是源；
//   * 处置：**不截断累加**，只在**显示层**加零值掩膜（半径 = 可调 R），不改重建。
// 故 D7 原版把点目标放在 (3,1)/(−5,4)（环内）是放错了工作区，其结论不适用于本项目。
//
// 本重跑：
//   1. 目标全部放在 ρ > R（真实工作区），覆盖近/中/远三个深度；
//   2. 量化环内伪影强度（对侧穿行射线的反投影）；
//   3. 给出「显示层零值掩膜」能买到多少质量指标改善；
//   4. DAS vs UBP 成对对比，两种成像模式。
//
// 编译：g++ -std=c++17 -O2 -I<paths> -o d7b d7b_outofring.cpp ring_recon.cpp RingPhantomForward.cpp
#include "ring_recon.h"
#include "RingPhantomForward.h"
#include "pa_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const double FS = 250e6, C_SOUND = 1490.0, R_RING = 6.57e-3, FOV = 36e-3;
static const int SAMP_DEPTH = 4000;
static const double PI = 3.14159265358979323846;

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

static pametric::Image toMetrics(const Img& r) {
    return pametric::Image{ r.v.data(), r.nx, r.ny, r.x0, r.y0, r.dx, r.dy };
}

// 显示层零值掩膜：ρ < maskRadius 的像素置 0（不改重建，只遮显示）
static Img applyDisplayMask(const Img& r, double maskRadius) {
    Img o = r;
    for (int iy = 0; iy < r.ny; ++iy)
        for (int ix = 0; ix < r.nx; ++ix) {
            const double x = r.x0 + ix * r.dx, y = r.y0 + iy * r.dy;
            if (std::sqrt(x * x + y * y) < maskRadius)
                o.v[(size_t)iy * r.nx + ix] = 0.0f;
        }
    return o;
}

// 环内伪影强度：ρ < R 内的 |I| 峰值，以及相对目标峰值的比值
static void reportInRingArtifact(const char* tag, const Img& r, double targetPeak) {
    double pkIn = 0, px = 0, py = 0;
    for (int iy = 0; iy < r.ny; ++iy)
        for (int ix = 0; ix < r.nx; ++ix) {
            const double x = r.x0 + ix * r.dx, y = r.y0 + iy * r.dy;
            if (std::sqrt(x * x + y * y) < R_RING) {
                const double a = std::fabs((double)r.v[(size_t)iy * r.nx + ix]);
                if (a > pkIn) { pkIn = a; px = x; py = y; }
            }
        }
    std::printf("    环内伪影峰值 %.4g @ (%.3f,%.3f) mm   伪影/目标峰值 = %.4g\n",
                pkIn, px * 1e3, py * 1e3, targetPeak > 0 ? pkIn / targetPeak : 0.0);
}

// 符号统计：负值能量占比 —— 权重符号问题的直接证据
static void reportSignStats(const Img& r) {
    double ePos = 0, eNeg = 0;
    long nPos = 0, nNeg = 0;
    for (float v : r.v) {
        const double a = std::fabs((double)v);
        if (v >= 0) { ePos += a; ++nPos; } else { eNeg += a; ++nNeg; }
    }
    const double tot = ePos + eNeg;
    std::printf("    符号统计：正能量 %.2f%% (%ld 像素)   负能量 %.2f%% (%ld 像素)\n",
                tot > 0 ? 100.0 * ePos / tot : 0, nPos,
                tot > 0 ? 100.0 * eNeg / tot : 0, nNeg);
}

static void metricsLine(const char* tag, const Img& r,
                        const pametric::Roi& tgt, const pametric::Roi& bg) {
    const pametric::Image im = toMetrics(r);
    const auto g = pametric::gcnr(im, tgt, bg);
    const auto b = pametric::backgroundNoise(im, bg);
    std::printf("    [%s] CR=%.2f dB  gCNR=%.4f  背景std=%.4g  CNR=%.4f\n",
                tag, pametric::contrastDb(im, bg), g.gcnr, b.sd,
                pametric::cnr(im, tgt, bg));
}

int main(int argc, char** argv) {
    const double gridSize = (argc > 1) ? std::atof(argv[1]) : 0.1e-3;
    const int nd = (argc > 2) ? std::atoi(argv[2]) : 360;

    std::printf("==================================================================\n");
    std::printf("D7 重跑：目标在环外真实工作区（内窥成像口径）\n");
    std::printf("R = %.3f mm  FOV = %.1f mm  gridSize = %.3f mm  A-line = %d\n",
                R_RING * 1e3, FOV * 1e3, gridSize * 1e3, nd);
    std::printf("声源全在环外；环内是对侧穿行射线的反投影伪影，不是源。\n");
    std::printf("==================================================================\n");

    // ---- 体模：三个点目标，全部在 ρ > R 的真实工作区 ----
    // 近 / 中 / 远三个深度，三个不同方位
    auto at = [](double rhoMm, double degMm) {
        ringphantom::Source s;
        s.x = rhoMm * 1e-3 * std::cos(degMm * PI / 180.0);
        s.y = rhoMm * 1e-3 * std::sin(degMm * PI / 180.0);
        return s;
    };
    ringphantom::Source A = at(9.0, 30.0);    A.amplitude = 1.00;
    ringphantom::Source B = at(13.0, 150.0);  B.amplitude = 0.80;
    ringphantom::Source C = at(17.0, 270.0);  C.amplitude = 0.60;
    std::printf("\n体模（全部环外）：\n");
    std::printf("  A = (%.3f, %.3f) mm  ρ=%.2f mm  幅值 %.2f\n", A.x*1e3, A.y*1e3,
                std::sqrt(A.x*A.x+A.y*A.y)*1e3, A.amplitude);
    std::printf("  B = (%.3f, %.3f) mm  ρ=%.2f mm  幅值 %.2f\n", B.x*1e3, B.y*1e3,
                std::sqrt(B.x*B.x+B.y*B.y)*1e3, B.amplitude);
    std::printf("  C = (%.3f, %.3f) mm  ρ=%.2f mm  幅值 %.2f\n", C.x*1e3, C.y*1e3,
                std::sqrt(C.x*C.x+C.y*C.y)*1e3, C.amplitude);

    ringphantom::Geometry g; g.fs = FS; g.c = C_SOUND; g.sampDepth = SAMP_DEPTH;
    std::vector<double> th(nd), rad(nd);
    for (int j = 0; j < nd; ++j) {
        th[(size_t)j] = (359.0 / (nd - 1) * j) * PI / 180.0;
        rad[(size_t)j] = R_RING;
    }

    std::vector<float> xv, yv;
    ringrecon::makeGrid(FOV, gridSize, xv, yv);

    // ROI：目标 A 附近 / 远背景区（环内一侧，无源）
    pametric::Roi tgtA{ A.x, A.y, 1.5e-3, 1.5e-3 };
    pametric::Roi bgIn{ 0.0, 11.0e-3, 2.5e-3, 2.5e-3 };   // 环外空白区（避开三个目标）

    struct Case { const char* name; ringphantom::PulseShape pulse; };
    const Case cases[2] = {
        {"Delta（定位用）", ringphantom::PulseShape::Delta},
        {"GaussDeriv（N 形，反演核差异）", ringphantom::PulseShape::GaussDeriv},
    };

    for (const Case& cs : cases) {
        ringphantom::ForwardParams fp;
        fp.pulse = cs.pulse;
        const auto bscan = ringphantom::simulate({A, B, C}, th, rad, g, fp);
        std::printf("\n########## 脉冲：%s ##########\n", cs.name);

        for (int mode = 0; mode < 2; ++mode) {
            for (int inv = 0; inv < 2; ++inv) {
                ringrecon::ReconParams rp;
                rp.fs = FS; rp.c = C_SOUND; rp.R = R_RING;
                rp.fov = FOV; rp.gridSize = gridSize;
                rp.distanceWeightExponent = 1.0; rp.minDistance = 0.0;
                rp.maskOutOfRange = true; rp.interpolation = "linear";
                if (mode == 0) { rp.fovDeg = 360.0; rp.fovTheta0Deg = 0.0; }
                else           { rp.fovDeg = 359.0; rp.fovTheta0Deg = 0.0; }
                rp.inversion = inv ? ringrecon_inv::InversionMode::Ubp
                                   : ringrecon_inv::InversionMode::Das;
                ringrecon::IncrementalState st;
                ringrecon::dasReconAppend(bscan, SAMP_DEPTH, nd, rp, 0.0, 359.0, xv, yv, st);
                const Img img = toRowMajor(st, xv, yv);

                const char* modeName = mode ? "扇区拼接" : "全局    ";
                const char* invName  = inv  ? "UBP" : "DAS";
                std::printf("\n  [%s | %s]\n", modeName, invName);

                // 全局峰值（应落在某个真目标上；若落在别处即被伪影主导）
                const pametric::Peak pk = pametric::findPeak(toMetrics(img));
                std::printf("    全局峰值 (%.3f,%.3f) mm  ρ=%.3f mm  值=%.4g\n",
                            pk.x*1e3, pk.y*1e3,
                            std::sqrt(pk.x*pk.x+pk.y*pk.y)*1e3, pk.value);

                // 各真目标处位置误差与相对幅度
                for (int t = 0; t < 3; ++t) {
                    const ringphantom::Source* s = (t == 0) ? &A : (t == 1) ? &B : &C;
                    pametric::Roi roi{ s->x, s->y, 1.5e-3, 1.5e-3 };
                    const pametric::Peak tp = pametric::findPeak(toMetrics(img));
                    // 目标邻域局部峰值
                    double best = -1, bx = 0, by = 0;
                    for (int iy = 0; iy < img.ny; ++iy)
                        for (int ix = 0; ix < img.nx; ++ix) {
                            const double x = img.x0 + ix*img.dx, y = img.y0 + iy*img.dy;
                            if (!roi.contains(x, y)) continue;
                            const double a = std::fabs((double)img.v[(size_t)iy*img.nx+ix]);
                            if (a > best) { best = a; bx = x; by = y; }
                        }
                    const double err = std::sqrt((bx-s->x)*(bx-s->x)+(by-s->y)*(by-s->y));
                    std::printf("    目标%c: 位置误差 %.4f mm  局部峰值 %.4g  /全局峰值 = %.4g\n",
                                'A'+t, err*1e3, best, best/std::fabs((double)pk.value));
                }
                reportSignStats(img);
                reportInRingArtifact("环内伪影", img, std::fabs((double)pk.value));

                // 指标：无掩膜 vs 显示掩膜（maskRadius = R）
                std::printf("    —— 质量指标（目标 A ROI）——\n");
                metricsLine("无掩膜  ", img, tgtA, bgIn);
                const Img masked = applyDisplayMask(img, R_RING);
                metricsLine("掩膜=R  ", masked, tgtA, bgIn);
            }
        }
    }
    std::printf("\n（只测不改生产代码；显示掩膜按规划为显示层零值遮盖，不改重建）\n");
    return 0;
}
