// d8_weight_variants.cpp —— 权重直接性因子的诊断对照（不改生产代码）
//
// 背景：内窥成像、源全在环外。现有 DAS 权重 w = Δθ·cosα/d 中
//   cosα = (R − r·n̂)/d 是相对**内向**法向的余弦；
//   环外像素的**同侧（最近、直达）**探测器 cosα = −1 ⇒ 负权重；
//            **对侧（最远、穿行射线）** cosα = +1 ⇒ 正权重。
//
// 关键：不能只做符号翻转 —— acc 整体取反只让图像变号，|I| 不变，等于没测。
// 该比的是**直接性因子怎么用**。UBP 里 cosα 是立体角/雅可比项，其符号表示
// 「源在探测面哪一侧」；源在环外时它必然变号。故本实验比四个变体：
//
//   V0  w = Δθ·cosα/d                        现有（内向法向，带符号）—— 基线
//   V1  w = Δθ·|cosα|/d                      只取幅度（同侧/对侧同权）
//   V2  w = Δθ·max(0, −cosα)/d               外向法向正部 ⇒ 只留同侧直达探测器，
//                                            排除对侧穿行射线（即环内伪影来源）
//   V3  w = Δθ/d                             无直接性项
//
// 自检：V0 必须复现生产 dasReconAppend（否则本诊断实现不可信）。
//
// 编译：g++ -std=c++17 -O2 -I<paths> -o d8 d8_weight_variants.cpp ring_recon.cpp RingPhantomForward.cpp
#include "ring_recon.h"
#include "RingPhantomForward.h"
#include "pa_metrics.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

enum Variant { V0_SIGNED = 0, V1_ABS = 1, V2_OUTWARD_POS = 2, V3_NODIR = 3 };
static const char* VNAME[4] = {
    "V0 现有 Δθ·cosα/d        ",
    "V1 幅度 Δθ·|cosα|/d      ",
    "V2 外向正部 Δθ·max(0,−cosα)/d",
    "V3 无直接性 Δθ/d         ",
};

// 诊断用反投影：几何/走时/插值逐项镜像 dasReconAppend，仅把权重做成可切换。
// V0 分支的表达式与生产逐字同形，用于自检。
static double g_gridSize = 0.1e-3;
static void bpDiag(const std::vector<float>& bscan, int nt, int nd,
                   double blockStartDeg, double blockSpanDeg,
                   const std::vector<float>& xv, const std::vector<float>& yv,
                   Variant variant, ringrecon::IncrementalState& st) {
    const int nx = (int)xv.size(), ny = (int)yv.size();
    if (nx < 2 || ny < 2 || nd < 2) return;
    if (st.acc.empty()) {
        st.acc.assign((size_t)nx * ny, 0.0f);
        st.accW.assign((size_t)nx * ny, 0.0f);
        st.nBlock = 0;
    }
    const float fs = (float)FS, c = (float)C_SOUND, pre = fs / c;
    const float R = (float)R_RING, R2 = R * R;
    const float minDist = (float)g_gridSize;   // minDistance=0 → 自动取网格步长
    const float wscale = -(float)(blockSpanDeg / (nd - 1) * PI / 180.0);
    std::vector<float> detx(nd), dety(nd);
    for (int j = 0; j < nd; ++j) {
        const float th = (float)((blockStartDeg + j * blockSpanDeg / (nd - 1)) * PI / 180.0);
        detx[(size_t)j] = R * std::cos(th);
        dety[(size_t)j] = R * std::sin(th);
    }
    for (int ix = 0; ix < nx; ++ix) {
        const float x = xv[(size_t)ix];
        for (int iy = 0; iy < ny; ++iy) {
            const float y = yv[(size_t)iy];
            const size_t idx = (size_t)ix * ny + iy;
            const float r2 = x * x + y * y, r2mR2 = r2 - R2;
            float acc = 0.0f, accw = 0.0f;
            for (int j = 0; j < nd; ++j) {
                const float proj = x * detx[(size_t)j] + y * dety[(size_t)j];
                const float dotp = proj - R2;
                const float dist2 = r2mR2 - 2.0f * dotp;
                const float dist = std::sqrt(std::max(dist2, 0.0f));
                const float dsafe = std::max(dist, minDist);
                const float tf = dist * pre;
                // 线性插值（与生产同型：1-based 下标）
                float vv = 0.0f;
                {
                    const float i0f = std::floor(tf);
                    const float frac = tf - i0f;
                    const int i0 = (int)i0f + 1;
                    const bool valid = (i0 >= 1) && (i0 <= nt - 1);
                    int i0c = std::max(std::min(i0, nt - 1), 1);
                    const float v0 = bscan[(size_t)(i0c - 1) + (size_t)j * nt];
                    const float v1 = bscan[(size_t)i0c + (size_t)j * nt];
                    vv = v0 + frac * (v1 - v0);
                    if (!valid) vv = 0.0f;
                }
                float w = 0.0f;
                switch (variant) {
                case V0_SIGNED:
                    w = wscale * dotp / (R * dsafe * dsafe);      // 生产同形
                    break;
                case V1_ABS:
                    w = std::fabs(wscale * dotp / (R * dsafe * dsafe));
                    break;
                case V2_OUTWARD_POS: {
                    // 外向法向正部：只保留「源在探测器外侧」的直达探测器
                    const float wv = wscale * dotp / (R * dsafe * dsafe);   // = Δθ·cosα_inward/d
                    w = std::max(0.0f, -wv);
                    break;
                }
                case V3_NODIR:
                    w = -wscale / dsafe;                          // Δθ/d，恒正
                    break;
                }
                acc += w * vv;
                accw += std::fabs(w);
            }
            st.acc[idx] += acc;
            st.accW[idx] += accw;
        }
    }
    ++st.nBlock;
}

static void report(const char* tag, const Img& r,
                   const std::vector<ringphantom::Source>& srcs,
                   const pametric::Roi& bg) {
    const pametric::Image im = toMetrics(r);
    const pametric::Peak pk = pametric::findPeak(im);
    std::printf("\n  [%s]\n", tag);
    std::printf("    全局峰值 (%.3f,%.3f) mm ρ=%.3f mm 值=%.4g\n",
                pk.x*1e3, pk.y*1e3, std::sqrt(pk.x*pk.x+pk.y*pk.y)*1e3, pk.value);
    double ePos = 0, eNeg = 0;
    for (float v : r.v) { const double a = std::fabs((double)v); if (v >= 0) ePos += a; else eNeg += a; }
    const double tot = ePos + eNeg;
    std::printf("    正/负能量 %.1f%% / %.1f%%\n",
                tot > 0 ? 100.0*ePos/tot : 0, tot > 0 ? 100.0*eNeg/tot : 0);
    double pkIn = 0;
    for (int iy = 0; iy < r.ny; ++iy)
        for (int ix = 0; ix < r.nx; ++ix) {
            const double x = r.x0 + ix*r.dx, y = r.y0 + iy*r.dy;
            if (std::sqrt(x*x + y*y) < R_RING)
                pkIn = std::max(pkIn, std::fabs((double)r.v[(size_t)iy*r.nx+ix]));
        }
    std::printf("    环内伪影/全局峰值 = %.4g\n", pk.value != 0 ? pkIn/std::fabs((double)pk.value) : 0.0);
    for (size_t t = 0; t < srcs.size(); ++t) {
        const auto& s = srcs[t];
        pametric::Roi roi{ s.x, s.y, 1.5e-3, 1.5e-3 };
        double best = -1, bx = 0, by = 0;
        for (int iy = 0; iy < r.ny; ++iy)
            for (int ix = 0; ix < r.nx; ++ix) {
                const double x = r.x0 + ix*r.dx, y = r.y0 + iy*r.dy;
                if (!roi.contains(x, y)) continue;
                const double a = std::fabs((double)r.v[(size_t)iy*r.nx+ix]);
                if (a > best) { best = a; bx = x; by = y; }
            }
        const double err = std::sqrt((bx-s.x)*(bx-s.x)+(by-s.y)*(by-s.y));
        std::printf("    目标%c 位置误差 %.4f mm  峰值/全局峰值 %.4g\n",
                    'A'+(int)t, err*1e3, pk.value!=0 ? best/std::fabs((double)pk.value) : 0.0);
    }
    pametric::Roi tgt{ srcs[0].x, srcs[0].y, 1.5e-3, 1.5e-3 };
    std::printf("    [目标A] CR=%.2f dB  gCNR=%.4f  背景std=%.4g  CNR=%.4f\n",
                pametric::contrastDb(im, bg), pametric::gcnr(im, tgt, bg).gcnr,
                pametric::backgroundNoise(im, bg).sd, pametric::cnr(im, tgt, bg));
}

int main(int argc, char** argv) {
    const double gridSize = (argc > 1) ? std::atof(argv[1]) : 0.1e-3;
    g_gridSize = gridSize;
    const int nd = (argc > 2) ? std::atoi(argv[2]) : 360;
    const double startDeg = 0.0, spanDeg = 359.0;

    std::printf("==================================================================\n");
    std::printf("权重直接性因子诊断对照（内窥：源全在环外）\n");
    std::printf("R=%.3f mm  FOV=%.1f mm  gridSize=%.3f mm  A-line=%d\n",
                R_RING*1e3, FOV*1e3, gridSize*1e3, nd);
    std::printf("==================================================================\n");

    auto at = [](double rhoMm, double deg) {
        ringphantom::Source s;
        s.x = rhoMm*1e-3*std::cos(deg*PI/180.0);
        s.y = rhoMm*1e-3*std::sin(deg*PI/180.0);
        return s;
    };
    std::vector<ringphantom::Source> srcs = { at(9.0,30.0), at(13.0,150.0), at(17.0,270.0) };
    srcs[0].amplitude = 1.00; srcs[1].amplitude = 0.80; srcs[2].amplitude = 0.60;

    ringphantom::Geometry g; g.fs = FS; g.c = C_SOUND; g.sampDepth = SAMP_DEPTH;
    std::vector<double> th(nd), rad(nd);
    for (int j = 0; j < nd; ++j) {
        th[(size_t)j] = (spanDeg/(nd-1)*j)*PI/180.0;
        rad[(size_t)j] = R_RING;
    }
    std::vector<float> xv, yv;
    ringrecon::makeGrid(FOV, gridSize, xv, yv);
    pametric::Roi bg{ 0.0, 11.0e-3, 2.5e-3, 2.5e-3 };

    ringphantom::ForwardParams fp; fp.pulse = ringphantom::PulseShape::Delta;
    const auto bscan = ringphantom::simulate(srcs, th, rad, g, fp);

    // ---- 自检：V0 必须复现生产 dasReconAppend ----
    {
        ringrecon::ReconParams rp;
        rp.fs = FS; rp.c = C_SOUND; rp.R = R_RING; rp.fov = FOV; rp.gridSize = gridSize;
        rp.distanceWeightExponent = 1.0; rp.minDistance = 0.0;
        rp.maskOutOfRange = true; rp.interpolation = "linear"; rp.fovDeg = 360.0;
        rp.inversion = ringrecon_inv::InversionMode::Das;
        ringrecon::IncrementalState prod, diag;
        ringrecon::dasReconAppend(bscan, SAMP_DEPTH, nd, rp, startDeg, spanDeg, xv, yv, prod);
        bpDiag(bscan, SAMP_DEPTH, nd, startDeg, spanDeg, xv, yv, V0_SIGNED, diag);
        const std::vector<float> a = ringrecon::normalizedImage(prod);
        const std::vector<float> b = ringrecon::normalizedImage(diag);
        double maxRel = 0, scale = 0;
        for (size_t k = 0; k < a.size(); ++k) scale = std::max(scale, (double)std::fabs(a[k]));
        for (size_t k = 0; k < a.size(); ++k)
            maxRel = std::max(maxRel, std::fabs((double)a[k] - (double)b[k]));
        std::printf("\n【自检】V0 vs 生产 dasReconAppend：最大绝对差 %.4g（图幅 %.4g，相对 %.3g）\n",
                    maxRel, scale, scale > 0 ? maxRel/scale : 0.0);
        std::printf("       %s\n", (scale > 0 ? maxRel/scale : 0.0) < 1e-5 ?
                    "通过：诊断实现可信" : "未通过：诊断实现不可信，后续结论作废");
    }

    for (int v = 0; v < 4; ++v) {
        ringrecon::IncrementalState st;
        bpDiag(bscan, SAMP_DEPTH, nd, startDeg, spanDeg, xv, yv, (Variant)v, st);
        report(VNAME[v], toRowMajor(st, xv, yv), srcs, bg);
    }
    std::printf("\n（只测不改生产代码）\n");
    return 0;
}
