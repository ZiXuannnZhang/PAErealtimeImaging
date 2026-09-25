// d7_phantom_ab.cpp — D7 合成体模上 B1（UBP 成对切换）相对现有 DAS 的收益上限评估
//
// 前向模型：**共享实现** ringphantom::simulate
//     来源 codex/das-dual-wavelength-quality-b-tier-20260925 @ 6846864ba120e5ecda0aaeb44fef723ae66bb7d3
//     契约测试 ring_phantom_forward_test 25/25 PASS（本机已复现）
// 重建：**生产 CPU 参考** ringrecon::dasReconAppend / normalizedImage 作**被测对象**，
//     由 ringrecon_inv::InversionMode 成对切换 Das / Ubp（前提 R2）。
// 度量：pa_metrics.h（D4 口径，独立实现，不调用生产函数）。
//
// 编译（无 Qt/CUDA）：
//   g++ -std=c++17 -O2 -o d7_phantom_ab d7_phantom_ab.cpp RingPhantomForward.cpp ring_recon.cpp
//
// 前向模型已知限制（补充 §6，不得当作能力宣称）：不建模折射/透射、分层声速、
// 有限探头尺寸、噪声（本文件单独加噪）、量化、fluence。
#include "RingPhantomForward.h"
#include "ring_recon.h"
#include "pa_metrics.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

// ---------------- 项目默认几何（ring_recon_cuda_set_defaults）----------------
static const double FS = 250e6;
static const double C_SOUND = 1490.0;     // 单声速（前向侧不支持分层，见限制）
static const double R_RING = 6.57e-3;
static double FOV = 36e-3;   // 可由命令行覆盖（argv[2]），用于 FOV 敏感性诊断
static const int    SAMP_DEPTH = 4000;
static const int    N_CHANNELS = 8;
static const int    K_PER_CHANNEL = 500;  // 每通道每波长每圈 A-line 数
static const double SECTOR_START_DEG = 180.0;
static const double SECTOR_WIDTH_DEG = 360.0 / N_CHANNELS;   // 45 deg

struct RunResult {
    std::vector<float> img;   // 行主序 [iy*nx+ix]
    int nx = 0, ny = 0;
    double x0 = 0, y0 = 0, dx = 0, dy = 0;
};

// 生产 IncrementalState 的索引是 idx = ix*ny + iy（x 外层）；转成行主序给度量用。
static RunResult toRowMajor(const ringrecon::IncrementalState& st,
                            const std::vector<float>& xv, const std::vector<float>& yv,
                            bool normalize) {
    RunResult r;
    r.nx = (int)xv.size(); r.ny = (int)yv.size();
    r.x0 = xv.front(); r.y0 = yv.front();
    r.dx = (r.nx > 1) ? (xv[1] - xv[0]) : 0.0;
    r.dy = (r.ny > 1) ? (yv[1] - yv[0]) : 0.0;
    r.img.assign((size_t)r.nx * r.ny, 0.0f);
    const std::vector<float> norm = normalize ? ringrecon::normalizedImage(st)
                                              : std::vector<float>();
    const std::vector<float>& s = normalize ? norm : st.acc;
    for (int ix = 0; ix < r.nx; ++ix)
        for (int iy = 0; iy < r.ny; ++iy)
            r.img[(size_t)iy * r.nx + ix] = s[(size_t)ix * r.ny + iy];
    return r;
}

static const double kPi = 3.14159265358979323846;

// 生成 A-line 角度/半径。
// 【关键契约】生产 dasReconAppend 并**不接收**逐线角度，而是由
//   stepDeg = blockSpanDeg/(nd-1),  th_j = blockStartDeg + j*stepDeg
// 自行推导（ring_recon.cpp:191,204）。因此前向必须按**完全相同**的公式生成角度，
// 否则前向与重建的探测器位置不一致（会导致重建焦点错位）。
// 这里取 stepDeg = 0.09 deg（= sectorWidth/K，与项目 MainWindow.cpp:5369 一致），
// 于是 blockSpanDeg = stepDeg*(nd-1)，保证两边逐线角度逐位相同。
static const double STEP_DEG = SECTOR_WIDTH_DEG / K_PER_CHANNEL;   // 0.09 deg

static void makeLines(std::vector<double>& theta, std::vector<double>& radii,
                      double blockStartDeg, int nd) {
    theta.clear(); radii.clear();
    theta.reserve(nd); radii.reserve(nd);
    for (int j = 0; j < nd; ++j) {
        theta.push_back((blockStartDeg + j * STEP_DEG) * kPi / 180.0);
        radii.push_back(R_RING);
    }
}
static double spanFor(int nd) { return STEP_DEG * (nd - 1); }

// splice 模式专用：逐通道前向 + 逐通道累加（每通道只在自己扇区的像素上累加）
static RunResult reconstructSplice(const std::vector<ringphantom::Source>& sources,
                                   ringrecon_inv::InversionMode inv,
                                   ringphantom::PulseShape pulse,
                                   double gridSize, double noiseSnr,
                                   unsigned seed) {
    std::vector<float> xv, yv;
    ringrecon::makeGrid(FOV, gridSize, xv, yv);
    ringrecon::ReconParams rp;
    rp.fs = FS; rp.c = C_SOUND; rp.R = R_RING;
    rp.fov = FOV; rp.gridSize = gridSize;
    rp.distanceWeightExponent = 1.0;
    rp.minDistance = 0.0;
    rp.maskOutOfRange = true;
    rp.interpolation = "linear";
    rp.inversion = inv;

    ringrecon::IncrementalState st;
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);

    ringphantom::Geometry geo; geo.fs = FS; geo.c = C_SOUND; geo.sampDepth = SAMP_DEPTH;
    ringphantom::ForwardParams fp; fp.pulse = pulse;

    for (int s = 0; s < N_CHANNELS; ++s) {
        const double start = SECTOR_START_DEG + s * SECTOR_WIDTH_DEG;
        std::vector<double> th, ra;
        makeLines(th, ra, start, K_PER_CHANNEL);
        std::vector<float> bscan = ringphantom::simulate(sources, th, ra, geo, fp);
        if (bscan.empty()) continue;
        if (noiseSnr > 0) {
            double pk = 0;
            for (float v : bscan) pk = std::max(pk, (double)std::fabs(v));
            const double sigma = pk / noiseSnr;
            for (auto& v : bscan) v += (float)(sigma * gauss(rng));
        }
        rp.fovDeg = SECTOR_WIDTH_DEG;
        rp.fovTheta0Deg = start;
        ringrecon::dasReconAppend(bscan, SAMP_DEPTH, (int)th.size(), rp,
                                  start, spanFor((int)th.size()), xv, yv, st);
    }
    return toRowMajor(st, xv, yv, true);
}

// global 模式专用（spliceMode=0：所有 A-line 对所有像素累加）
static RunResult reconstructGlobal(const std::vector<ringphantom::Source>& sources,
                                   ringrecon_inv::InversionMode inv,
                                   ringphantom::PulseShape pulse,
                                   double gridSize, double noiseSnr, unsigned seed) {
    const int nd = N_CHANNELS * K_PER_CHANNEL;
    std::vector<double> th, ra;
    makeLines(th, ra, SECTOR_START_DEG, nd);
    ringphantom::Geometry geo; geo.fs = FS; geo.c = C_SOUND; geo.sampDepth = SAMP_DEPTH;
    ringphantom::ForwardParams fp; fp.pulse = pulse;
    std::vector<float> bscan = ringphantom::simulate(sources, th, ra, geo, fp);
    if (noiseSnr > 0) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> gauss(0.0, 1.0);
        double pk = 0;
        for (float v : bscan) pk = std::max(pk, (double)std::fabs(v));
        const double sigma = pk / noiseSnr;
        for (auto& v : bscan) v += (float)(sigma * gauss(rng));
    }
    std::vector<float> xv, yv;
    ringrecon::makeGrid(FOV, gridSize, xv, yv);
    ringrecon::ReconParams rp;
    rp.fs = FS; rp.c = C_SOUND; rp.R = R_RING;
    rp.fov = FOV; rp.gridSize = gridSize;
    rp.distanceWeightExponent = 1.0;
    rp.minDistance = 0.0;
    rp.maskOutOfRange = true;
    rp.interpolation = "linear";
    rp.inversion = inv;
    rp.fovDeg = 360.0; rp.fovTheta0Deg = 0.0;
    ringrecon::IncrementalState st;
    ringrecon::dasReconAppend(bscan, SAMP_DEPTH, nd, rp,
                              SECTOR_START_DEG, spanFor(nd), xv, yv, st);
    return toRowMajor(st, xv, yv, true);
}

// 伪影定位诊断：全局峰值落在哪个半径；|图像| 能量有多少落在探测环附近。
// D3 预言 UBP 的 |dΩ| ~ R·Δθ·cosα/d² 在 d→0 处按 1/d² 发散（相对环心放大 (R/minDist)²），
// 故 UBP 图应被环带主导，DAS（1/d）应弱得多。本函数直接量化这件事。
struct ArtifactDiag { double peakRadiusMm = 0; double ringBandFrac = 0; double ringBand = 1.0e-3; };
static ArtifactDiag diagnoseArtifacts(const RunResult& r) {
    ArtifactDiag d;
    double tot = 0, ring = 0, bestAbs = 0;
    for (int iy = 0; iy < r.ny; ++iy) {
        const double y = r.y0 + iy * r.dy;
        for (int ix = 0; ix < r.nx; ++ix) {
            const double x = r.x0 + ix * r.dx;
            const double v = std::fabs((double)r.img[(std::size_t)iy * r.nx + ix]);
            tot += v;
            const double rho = std::hypot(x, y);
            if (std::fabs(rho - R_RING) < d.ringBand) ring += v;
            if (v > bestAbs) { bestAbs = v; d.peakRadiusMm = rho * 1e3; }
        }
    }
    d.ringBandFrac = tot > 0 ? ring / tot : 0;
    return d;
}

static pametric::Image asImage(const RunResult& r) {
    return pametric::Image{ r.img.data(), r.nx, r.ny, r.x0, r.y0, r.dx, r.dy };
}

// 局部峰值：在真值点邻域内取 |v| 最大像素（全局峰值会被更强的目标抢走，
// 用它算“到目标 B 的位置误差”没有意义）。
static pametric::Peak localPeak(const RunResult& r, double cx, double cy, double halfWin) {
    pametric::Peak p;
    for (int iy = 0; iy < r.ny; ++iy) {
        const double y = r.y0 + iy * r.dy;
        if (std::fabs(y - cy) > halfWin) continue;
        for (int ix = 0; ix < r.nx; ++ix) {
            const double x = r.x0 + ix * r.dx;
            if (std::fabs(x - cx) > halfWin) continue;
            const float v = r.img[(std::size_t)iy * r.nx + ix];
            if (!p.found || std::fabs(v) > std::fabs(p.value)) {
                p.found = true; p.value = v; p.ix = ix; p.iy = iy; p.x = x; p.y = y;
            }
        }
    }
    return p;
}


int main(int argc, char** argv) {
    double gridSize = 0.1e-3;
    if (argc > 1) gridSize = std::atof(argv[1]);
    if (argc > 2) FOV = std::atof(argv[2]);
    const int nAlinesTotal = N_CHANNELS * K_PER_CHANNEL;

    std::printf("==================================================================\n");
    std::printf("D7  合成体模上 B1（UBP 成对切换）收益上限评估\n");
    std::printf("==================================================================\n\n");
    std::printf("前向模型：共享 RingPhantomForward @ 6846864ba120e5ecda0aaeb44fef723ae66bb7d3\n");
    std::printf("          （ring_phantom_forward_test 25/25 PASS，本机已复现）\n");
    std::printf("被测对象：生产 CPU 参考 ringrecon::dasReconAppend（inversion=Das/Ubp 成对切换）\n");
    std::printf("度量工具：pa_metrics.h（D4 口径）\n\n");
    std::printf("几何：fs=%.3g Hz  c=%.1f m/s  R=%.4f mm  fov=%.1f mm  sampDepth=%d\n",
                FS, C_SOUND, R_RING * 1e3, FOV * 1e3, SAMP_DEPTH);
    std::printf("      A-line 数 = %d (=%d 通道 x %d)，角步长 %.4f deg，sectorStart=%.1f deg\n",
                nAlinesTotal, N_CHANNELS, K_PER_CHANNEL,
                SECTOR_WIDTH_DEG / K_PER_CHANNEL, SECTOR_START_DEG);
    std::printf("      gridSize = %.4f mm（网格 %d x %d）\n",
                gridSize * 1e3, (int)std::ceil(FOV / gridSize), (int)std::ceil(FOV / gridSize));
    std::printf("      脉冲：GaussDeriv（N 形，默认，用于反演核差异）与 Delta（冲激，用于定位）\n");
    std::printf("\n【与项目默认值的差异，显式标注】\n");
    std::printf("  * gridSize 用 %.4f mm，项目默认 0.01 mm（10 um）。改粗是计算量取舍；\n", gridSize * 1e3);
    std::printf("    位置误差/分辨率的量化下限因此为一个网格步长。见文末收敛性核对。\n");
    std::printf("  * 声速用单声速 %.1f m/s。项目重建侧支持分层声速{1490,1540}，但共享前向\n", C_SOUND);
    std::printf("    不支持（补充 §6）；不得拿单速前向配分层重建，故两侧一致用单速。\n");
    std::printf("  * 网格坐标用 ringrecon::makeGrid（linspace(-fov/2, fov/2, N)），与 CUDA 核的\n");
    std::printf("    x=-fov/2+(i+0.5)*gridSize 略有差异（后者是半格偏移）。\n\n");

    // ---------------- 体模（已知真值）----------------
    // 三个已知真值要素分开成两个场景，避免“点目标 + 均匀盘”混在一个体模里时
    // 峰值归属不明确（度量的 peak 定义会被盘体的能量总量带偏）。
    //   场景点（P）：两个点目标  -> 位置误差、轴向/切向 -6 dB、真值处相对幅度
    //   场景盘（D）：一个均匀盘  -> CR、gCNR、CNR、背景噪声
    //   双波长：两个场景各自生成 wl2 = 0.4*wl1 -> 比值守卫
    const double P1x = 3.0e-3, P1y = 1.0e-3;
    const double P2x = -5.0e-3, P2y = 4.0e-3;
    const double DISKx = 0.0, DISKy = -8.0e-3, DISKr = 2.0e-3;
    const double A1 = 1.0;
    const double WL_RATIO = 0.4;   // wl2 = 0.4 * wl1（已知真值幅值比）

    auto buildPoints = [&](double ampScale) {
        std::vector<ringphantom::Source> s;
        s.push_back({P1x, P1y, A1 * ampScale});
        s.push_back({P2x, P2y, 0.8 * A1 * ampScale});
        return s;
    };
    auto buildDisk = [&](double ampScale) {
        return ringphantom::uniformDisk(DISKx, DISKy, DISKr, A1 * ampScale, 4);
    };
    const auto ptsWL1 = buildPoints(1.0), ptsWL2 = buildPoints(WL_RATIO);
    const auto dskWL1 = buildDisk(1.0),   dskWL2 = buildDisk(WL_RATIO);
    std::printf("体模真值：\n");
    std::printf("  [场景 P] 点目标 A = (%.3f, %.3f) mm, 幅值 %.3f\n", P1x * 1e3, P1y * 1e3, A1);
    std::printf("  [场景 P] 点目标 B = (%.3f, %.3f) mm, 幅值 %.3f\n", P2x * 1e3, P2y * 1e3, 0.8 * A1);
    std::printf("  [场景 D] 均匀盘   = 中心 (%.3f, %.3f) mm, 半径 %.3f mm, 幅值 %.3f, 点数 %d\n",
                DISKx * 1e3, DISKy * 1e3, DISKr * 1e3, A1, (int)dskWL1.size());
    std::printf("  双波长幅值比 wl2/wl1 = %.4f（已知真值，两场景同用）\n\n", WL_RATIO);

    // ROI（显式，不硬编码到体模之外）
    pametric::Roi tgtDisk{ DISKx, DISKy, DISKr, DISKr };
    pametric::Roi bgRoi{ 12.0e-3, 12.0e-3, 3.0e-3, 3.0e-3 };   // 远离所有目标
    std::printf("ROI（显式打印，参数化，不绑定具体体模形状）：\n");
    std::printf("  目标 ROI %s\n", tgtDisk.describe().c_str());
    std::printf("  背景 ROI %s\n", bgRoi.describe().c_str());
    std::printf("  比值守卫 ROI %s\n\n", pametric::Roi{0, 0, 18e-3, 18e-3}.describe().c_str());

    struct Case { const char* pulseName; ringphantom::PulseShape pulse; };
    const Case cases[2] = {
        {"Delta     （冲激，用于定位/位置误差）", ringphantom::PulseShape::Delta},
        {"GaussDeriv（N 形，用于反演核差异）  ", ringphantom::PulseShape::GaussDeriv},
    };
    const char* modeName[2] = {"spliceMode=0（全局反投影）", "spliceMode=1（逐通道扇区拼接）"};
    const char* invName[2] = {"DAS（现有）", "UBP（B1 成对）"};

    // ---------------- 无噪 A/B ----------------
    std::printf("=== D7.1 无噪（理想、带宽充足）条件下的 A/B ===\n\n");
    for (const auto& cs : cases) {
        std::printf("--- 脉冲 %s ---\n", cs.pulseName);
        std::printf("\n[场景 P：两个点目标] 位置误差 / 分辨率 / 真值处相对幅度\n");
        std::printf("| 模式 | 反演 | errA(mm) | errB(mm) | 轴向-6dB | 切向-6dB | 峰值 | A处/峰值 | B处/峰值 | 峰值半径mm | 环带能量占比 |\n");
        for (int mode = 0; mode < 2; ++mode) {
            for (int iv = 0; iv < 2; ++iv) {
                const auto inv = iv == 0 ? ringrecon_inv::InversionMode::Das
                                         : ringrecon_inv::InversionMode::Ubp;
                RunResult rr = (mode == 0)
                    ? reconstructGlobal(ptsWL1, inv, cs.pulse, gridSize, 0.0, 1)
                    : reconstructSplice(ptsWL1, inv, cs.pulse, gridSize, 0.0, 1);
                const auto im = asImage(rr);
                const auto pk = pametric::findPeak(im);
                const auto pkA = localPeak(rr, P1x, P1y, 2.0e-3);
                const auto pkB = localPeak(rr, P2x, P2y, 2.0e-3);
                const double errA = pametric::positionErrorMm(pkA, P1x, P1y);
                const double errB = pametric::positionErrorMm(pkB, P2x, P2y);
                const auto res = pametric::resolution(im, pkA.x, pkA.y, 0.0, 0.0);
                auto at = [&](double x, double y) {
                    const int ix = (int)std::lround((x - rr.x0) / rr.dx);
                    const int iy = (int)std::lround((y - rr.y0) / rr.dy);
                    if (ix < 0 || iy < 0 || ix >= rr.nx || iy >= rr.ny) return 0.0;
                    return (double)rr.img[(size_t)iy * rr.nx + ix];
                };
                const double vA = at(P1x, P1y), vB = at(P2x, P2y);
                const auto ad = diagnoseArtifacts(rr);
                const double pkAbs = std::fabs(pk.value);
                std::printf("| %s | %s | %.4f | %.4f | %.4f | %.4f | %.4g | %.4f | %.4f | %.4f | %.6f |\n",
                            modeName[mode], invName[iv], errA, errB,
                            res.axialMm, res.tangentialMm, pk.value,
                            pkAbs > 0 ? vA / pkAbs : 0, pkAbs > 0 ? vB / pkAbs : 0,
                            ad.peakRadiusMm, ad.ringBandFrac);
            }
        }
        std::printf("\n[场景 D：均匀盘] CR / gCNR / 背景噪声 / CNR\n");
        std::printf("| 模式 | 反演 | CR(dB) | gCNR | 背景std | 背景std/峰值 | CNR | 峰值 | 盘心/峰值 |\n");
        for (int mode = 0; mode < 2; ++mode) {
            for (int iv = 0; iv < 2; ++iv) {
                const auto inv = iv == 0 ? ringrecon_inv::InversionMode::Das
                                         : ringrecon_inv::InversionMode::Ubp;
                RunResult rr = (mode == 0)
                    ? reconstructGlobal(dskWL1, inv, cs.pulse, gridSize, 0.0, 1)
                    : reconstructSplice(dskWL1, inv, cs.pulse, gridSize, 0.0, 1);
                const auto im = asImage(rr);
                const auto pk = pametric::findPeak(im);
                const double cr = pametric::contrastDb(im, bgRoi);
                const auto gr = pametric::gcnr(im, tgtDisk, bgRoi, 64);
                const auto bn = pametric::backgroundNoise(im, bgRoi);
                const double c = pametric::cnr(im, tgtDisk, bgRoi);
                const int ix = (int)std::lround((DISKx - rr.x0) / rr.dx);
                const int iy = (int)std::lround((DISKy - rr.y0) / rr.dy);
                const double vC = rr.img[(size_t)iy * rr.nx + ix];
                std::printf("| %s | %s | %.2f | %.4f | %.4g | %.4g | %.3f | %.4g | %.4f |\n",
                            modeName[mode], invName[iv], cr, gr.gcnr,
                            bn.sd, bn.relativeToPeak, c, pk.value,
                            std::fabs(pk.value) > 0 ? vC / std::fabs(pk.value) : 0);
            }
        }
        std::printf("\n");
    }

    // ---------------- 双波长比值守卫 ----------------
    std::printf("=== D7.2 双波长比值守卫（R1 判别性守卫）===\n\n");
    std::printf("同一重建算子分别作用于 wl1 / wl2，断言 I1/I2 = 已知真值 %.4f。\n", 1.0 / WL_RATIO);
    std::printf("| 模式 | 反演 | 脉冲 | 守卫结果 | meanRatio | relStd | relErrVs真值 |\n");
    for (int mode = 0; mode < 2; ++mode) {
        for (int iv = 0; iv < 2; ++iv) {
            const auto inv = iv == 0 ? ringrecon_inv::InversionMode::Das
                                     : ringrecon_inv::InversionMode::Ubp;
            for (const auto& cs : cases) {
                RunResult r1 = (mode == 0)
                    ? reconstructGlobal(ptsWL1, inv, cs.pulse, gridSize, 0.0, 2)
                    : reconstructSplice(ptsWL1, inv, cs.pulse, gridSize, 0.0, 2);
                RunResult r2 = (mode == 0)
                    ? reconstructGlobal(ptsWL2, inv, cs.pulse, gridSize, 0.0, 2)
                    : reconstructSplice(ptsWL2, inv, cs.pulse, gridSize, 0.0, 2);
                const auto g = pametric::dualWavelengthRatioGuard(
                    asImage(r1), asImage(r2), pametric::Roi{0, 0, 18e-3, 18e-3}, 1.0 / WL_RATIO);
                std::printf("| %s | %s | %s | %s | %.6g | %.3g | %.3g |\n",
                            modeName[mode], invName[iv],
                            cs.pulse == ringphantom::PulseShape::Delta ? "Delta" : "GaussDeriv",
                            g.ok ? "PASS" : "FAIL", g.meanRatio, g.relStd, g.relErrVsExpected);
            }
        }
    }
    std::printf("\n结论：两种反演、两种模式下比值守卫均应 PASS —— 权重与信号项都只含几何量，\n");
    std::printf("两波长同除 accW（纯几何）故比值不变（前提 R1 / G3）。\n\n");

    // ---------------- 加噪趋势 ----------------
    std::printf("=== D7.3 加噪后的变化趋势 ===\n\n");
    std::printf("噪声口径：高斯白噪，按 A-line 峰值 / 噪声 RMS = SNR 定标。\n");
    std::printf("D1 实测参考：testdata/01 Card1_ChA 噪声底 RMS≈22.4、回波峰值均≈20626 => SNR≈919。\n");
    std::printf("故 SNR=919 对应 D1 实测噪声水平；其余为趋势扫描。\n\n");
    std::printf("口径：场景 P（两个点目标）。gCNR / CNR 属盘体场景，见 D7.1 场景 D 表。\n");
    std::printf("| 脉冲 | 模式 | 反演 | SNR | 位置误差A(mm) | 轴向-6dB(mm) | 切向-6dB(mm) | 背景std | 峰值 |\n");
    for (const auto& cs : cases) {
        for (int mode = 0; mode < 2; ++mode) {
            for (int iv = 0; iv < 2; ++iv) {
                const auto inv = iv == 0 ? ringrecon_inv::InversionMode::Das
                                         : ringrecon_inv::InversionMode::Ubp;
                for (double snr : {919.0, 300.0, 100.0, 30.0}) {
                    RunResult rr = (mode == 0)
                        ? reconstructGlobal(ptsWL1, inv, cs.pulse, gridSize, snr, 7)
                        : reconstructSplice(ptsWL1, inv, cs.pulse, gridSize, snr, 7);
                    const auto im = asImage(rr);
                    const auto pk = pametric::findPeak(im);
                    const auto pkA = localPeak(rr, P1x, P1y, 2.0e-3);
                    const double errA = pametric::positionErrorMm(pkA, P1x, P1y);
                    const auto res = pametric::resolution(im, pkA.x, pkA.y, 0.0, 0.0);
                    const auto bn = pametric::backgroundNoise(im, bgRoi);
                    std::printf("| %s | %s | %s | %g | %.4f | %.4f | %.4f | %.4g | %.4g |\n",
                                cs.pulse == ringphantom::PulseShape::Delta ? "Delta" : "GaussDeriv",
                                modeName[mode], invName[iv], snr,
                                errA, res.axialMm, res.tangentialMm, bn.sd, pk.value);
                }
            }
        }
    }

    // ---------------- 收敛性核对 ----------------
    std::printf("\n=== D7.4 网格收敛性核对 ===\n\n");
    std::printf("口径：场景 P、脉冲 Delta、global 模式、无噪。\n");
    std::printf("| gridSize(mm) | 反演 | 位置误差A(mm) | 轴向-6dB(mm) | 切向-6dB(mm) |\n");
    for (double g : {0.2e-3, 0.1e-3, 0.05e-3}) {
        for (int iv = 0; iv < 2; ++iv) {
            const auto inv = iv == 0 ? ringrecon_inv::InversionMode::Das
                                     : ringrecon_inv::InversionMode::Ubp;
            RunResult rr = reconstructGlobal(ptsWL1, inv, ringphantom::PulseShape::Delta, g, 0.0, 3);
            const auto im = asImage(rr);
            const auto pk = pametric::findPeak(im);
            const double errA = pametric::positionErrorMm(pk, P1x, P1y);
            const auto res = pametric::resolution(im, pk.x, pk.y, 0.0, 0.0);
            std::printf("| %.3f | %s | %.4f | %.4f | %.4f |\n",
                        g * 1e3, invName[iv], errA, res.axialMm, res.tangentialMm);
        }
    }

    std::printf("\n=== 未验证项 / 限制 ===\n");
    std::printf("* 前向模型不建模：折射/透射、分层声速、有限探头尺寸、量化、fluence（补充 §6）。\n");
    std::printf("* 本结果是**模型自洽**的合成 A/B，不是实机效果预估；D7 不是闸门（原规格 §1）。\n");
    std::printf("* 不预设优劣：若 UBP 在某些指标上更差，表中如实呈现。\n");
    std::printf("* 绝对量级不可比（补充 §4）：UBP 输出 b=2p-2t*p' 的量纲与 DAS 的 p 不同，\n");
    std::printf("  峰值列只作同模式内参考；可比的是空间分布、相对幅度与质量指标。\n");
    std::printf("* gridSize 非项目默认 10 um；位置误差/分辨率受网格量化下限约束。\n");
    std::printf("* real hardware validation：D7 为合成数据，不涉实机。\n");
    std::printf("* source/code correctness：前向模型正确性见其 25/25 契约测试；\n");
    std::printf("  本文件不声称生产重建的正确性，只把它当被测对象。\n");
    return 0;
}
