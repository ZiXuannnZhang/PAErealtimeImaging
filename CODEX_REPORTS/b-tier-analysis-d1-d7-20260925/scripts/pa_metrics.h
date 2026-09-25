// pa_metrics.h — D4 图像质量度量工具（独立实现，不调用任何生产函数）
//
// 指标定义严格按 TASKS/D4图像质量度量工具_20260925-033000.md §3，不改口径。
//
// 硬约束（前提 R1/R3）：
//   * 度量在**输入的原始线性数值**上计算，不做任何隐式归一化；
//   * 不做逐图峰值归一化、不做显示拉伸、不做硬整流、不做 Hilbert 取模；
//   * 双波长比值守卫是判别性测试，会直接判死「逐图归一化/整流」类方案。
//
// 独立性：本头文件只用 <vector>/<cmath>/<cstdint>，不 include 任何生产/重建头。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pametric {

struct Roi {
    double cx = 0.0, cy = 0.0;   // 中心 [m]
    double halfW = 0.0, halfH = 0.0;  // 半宽/半高 [m]（轴对齐矩形）
    bool contains(double x, double y) const {
        return std::fabs(x - cx) <= halfW && std::fabs(y - cy) <= halfH;
    }
    std::string describe() const {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "ROI(c=(%.6f,%.6f) m, halfW=%.6f m, halfH=%.6f m)",
                      cx, cy, halfW, halfH);
        return std::string(buf);
    }
};

// 图像：行主序 img[iy*nx + ix]，坐标 x = x0 + ix*dx, y = y0 + iy*dy
struct Image {
    const float* data = nullptr;
    int nx = 0, ny = 0;
    double x0 = 0, y0 = 0, dx = 0, dy = 0;
    double x(int ix) const { return x0 + ix * dx; }
    double y(int iy) const { return y0 + iy * dy; }
    float at(int ix, int iy) const { return data[iy * nx + ix]; }
};

// ---------- 基础统计 ----------
struct Stats { double mean = 0, sd = 0, min = 0, max = 0; long n = 0; };

inline Stats stats(const Image& im, const Roi& r) {
    Stats s;
    double sum = 0, sum2 = 0, mn = 0, mx = 0;
    long n = 0;
    for (int iy = 0; iy < im.ny; ++iy) {
        const double yy = im.y(iy);
        for (int ix = 0; ix < im.nx; ++ix) {
            const double xx = im.x(ix);
            if (!r.contains(xx, yy)) continue;
            const double v = im.at(ix, iy);
            if (n == 0) { mn = mx = v; }
            sum += v; sum2 += v * v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            ++n;
        }
    }
    s.n = n;
    if (n > 0) {
        s.mean = sum / n;
        s.sd = std::sqrt(std::max(0.0, sum2 / n - s.mean * s.mean));
        s.min = mn; s.max = mx;
    }
    return s;
}

// ---------- 峰值 ----------
struct Peak { double x = 0, y = 0; float value = 0; int ix = 0, iy = 0; bool found = false; };

inline Peak findPeak(const Image& im) {
    Peak p;
    for (int iy = 0; iy < im.ny; ++iy)
        for (int ix = 0; ix < im.nx; ++ix) {
            const float v = im.at(ix, iy);
            if (!p.found || std::fabs(v) > std::fabs(p.value)) {
                p.found = true; p.value = v; p.ix = ix; p.iy = iy;
                p.x = im.x(ix); p.y = im.y(iy);
            }
        }
    return p;
}

// ---------- 1. 轴向/切向分辨率（−6 dB 全宽）----------
// 方向以**相对环心的径向/切向**定义：径向 = 过目标峰值点沿 r_hat 方向，
// 切向 = 沿 t_hat（r_hat 旋转 90°）。线性插值取半高交点。
// 注意：环心在 (0,0)。若目标恰在环心，径向未定义，返回 nan。
struct Resolution { double axialMm = 0, tangentialMm = 0; bool okAx = false, okTan = false; };

// 沿方向 (ux,uy) 从峰值点取线剖，求 |v| 降到峰值一半（-6 dB）的全宽
inline double widthAtMinus6dB(const Image& im, double px, double py,
                              double ux, double uy, bool& ok) {
    ok = false;
    // 找峰值点最近的像素值作参考峰值
    const int ix0 = (int)std::lround((px - im.x0) / im.dx);
    const int iy0 = (int)std::lround((py - im.y0) / im.dy);
    if (ix0 < 0 || ix0 >= im.nx || iy0 < 0 || iy0 >= im.ny) return 0;
    const double peak = std::fabs(im.at(ix0, iy0));
    if (peak <= 0) return 0;
    const double half = peak * 0.5;   // -6.02 dB
    const double step = std::min(im.dx, im.dy) * 0.5;
    // 向两侧步进，线性插值求交点
    auto cross = [&](double sgn) {
        double prev = peak;
        for (double s = step; s < 0.05; s += step) {
            const double xx = px + sgn * s * ux, yy = py + sgn * s * uy;
            const double gx = (xx - im.x0) / im.dx, gy = (yy - im.y0) / im.dy;
            const int i = (int)std::floor(gx), j = (int)std::floor(gy);
            if (i < 0 || j < 0 || i + 1 >= im.nx || j + 1 >= im.ny) return s;
            // 双线性插值
            const double fx = gx - i, fy = gy - j;
            const double v00 = im.at(i, j), v10 = im.at(i + 1, j);
            const double v01 = im.at(i, j + 1), v11 = im.at(i + 1, j + 1);
            const double v = (1 - fx) * (1 - fy) * v00 + fx * (1 - fy) * v10 +
                             (1 - fx) * fy * v01 + fx * fy * v11;
            const double av = std::fabs(v);
            if (av <= half) {
                if (prev <= 0) return s;
                const double t = (half - av) / (prev - av);
                return s - step + t * step;
            }
            prev = av;
        }
        return 0.05;
    };
    const double a = cross(-1.0), b = cross(+1.0);
    ok = (a > 0 && b > 0);
    return a + b;
}

inline Resolution resolution(const Image& im, double px, double py, double ringCx, double ringCy) {
    Resolution r;
    const double rx = px - ringCx, ry = py - ringCy;
    const double rn = std::hypot(rx, ry);
    if (rn <= 0) return r;
    const double ax = rx / rn, ay = ry / rn;      // 径向（轴向）
    const double tx = -ay, ty = ax;               // 切向
    bool ok1 = false, ok2 = false;
    r.axialMm = widthAtMinus6dB(im, px, py, ax, ay, ok1) * 1e3;
    r.tangentialMm = widthAtMinus6dB(im, px, py, tx, ty, ok2) * 1e3;
    r.okAx = ok1; r.okTan = ok2;
    return r;
}

// ---------- 2. 位置误差 ----------
inline double positionErrorMm(const Peak& p, double trueX, double trueY) {
    return std::hypot(p.x - trueX, p.y - trueY) * 1e3;
}

// ---------- 3. CR 对比度 ----------
// CR = 20*log10(peak / mean_background), dB
inline double contrastDb(const Image& im, const Roi& bg) {
    const Peak p = findPeak(im);
    const Stats s = stats(im, bg);
    if (s.n == 0 || s.mean == 0) return 0;
    return 20.0 * std::log10(std::fabs(p.value) / std::fabs(s.mean));
}

// ---------- 4. gCNR (Kempski 2020) ----------
// gCNR = 1 - integral min(pdf_T, pdf_B)
// 直方图口径：bin 数 = nbins；灰度归一化 = 两 ROI 合并的 [min,max] 线性映射到 [0,1]。
struct GCNRResult { double gcnr = 0; int nbins = 0; double lo = 0, hi = 1; };

inline GCNRResult gcnr(const Image& im, const Roi& target, const Roi& bg, int nbins = 64) {
    GCNRResult r;
    r.nbins = nbins;
    std::vector<double> tv, bv;
    tv.reserve(4096); bv.reserve(4096);
    double lo = 0, hi = 0; bool first = true;
    for (int iy = 0; iy < im.ny; ++iy) {
        for (int ix = 0; ix < im.nx; ++ix) {
            const double xx = im.x(ix), yy = im.y(iy);
            const double v = im.at(ix, iy);
            if (target.contains(xx, yy)) tv.push_back(v);
            else if (bg.contains(xx, yy)) bv.push_back(v);
            if (target.contains(xx, yy) || bg.contains(xx, yy)) {
                if (first) { lo = hi = v; first = false; }
                else { lo = std::min(lo, v); hi = std::max(hi, v); }
            }
        }
    }
    if (tv.empty() || bv.empty() || hi <= lo) return r;
    r.lo = lo; r.hi = hi;
    std::vector<double> ht(nbins, 0), hb(nbins, 0);
    const double scale = nbins / (hi - lo);
    for (double v : tv) {
        int b = (int)((v - lo) * scale);
        if (b < 0) b = 0;
        if (b >= nbins) b = nbins - 1;
        ht[b] += 1.0;
    }
    for (double v : bv) {
        int b = (int)((v - lo) * scale);
        if (b < 0) b = 0;
        if (b >= nbins) b = nbins - 1;
        hb[b] += 1.0;
    }
    double overlap = 0;
    for (int i = 0; i < nbins; ++i) {
        const double pt = ht[i] / tv.size(), pb = hb[i] / bv.size();
        overlap += std::min(pt, pb);
    }
    r.gcnr = 1.0 - overlap;
    return r;
}

// ---------- 5. 背景噪声 ----------
struct BgNoise { double sd = 0, relativeToPeak = 0; };

inline BgNoise backgroundNoise(const Image& im, const Roi& bg) {
    BgNoise b;
    const Stats s = stats(im, bg);
    const Peak p = findPeak(im);
    b.sd = s.sd;
    b.relativeToPeak = (std::fabs(p.value) > 0) ? s.sd / std::fabs(p.value) : 0;
    return b;
}

// ---------- 6. CNR ----------
// CNR = |mean_T - mean_B| / std_B
inline double cnr(const Image& im, const Roi& target, const Roi& bg) {
    const Stats t = stats(im, target), b = stats(im, bg);
    if (b.sd == 0) return 0;
    return std::fabs(t.mean - b.mean) / b.sd;
}

// ---------- 必选附加项：双波长比值守卫（R1 判别性测试）----------
// 断言 I1/I2 在 ROI 内为常数（相对标准差 <= 容差），且该常数等于输入幅值比。
//
// relFloor：|I2| 低于 relFloor*max|I2| 的像素跳过。这是**数值精度**保护，
// 不是归一化 —— 在信号为 0 的像素上做除法，比值只反映舍入误差，会让守卫
// 变得与被测对象无关。跳过判据只依赖几何/数值下限，与波长无关，故不违反 R1。
struct RatioGuard {
    bool ok = false;
    double meanRatio = 0;
    double relStd = 0;        // std/|mean|
    double expected = 0;      // 期望的幅值比
    double relErrVsExpected = 0;
    long n = 0;
    long nSkipped = 0;
    std::string detail;
};

inline RatioGuard dualWavelengthRatioGuard(const Image& i1, const Image& i2,
                                           const Roi& roi, double expectedRatio,
                                           double tolRelStd = 0.02,
                                           double tolRelErr = 0.02,
                                           double relFloor = 1e-3) {
    RatioGuard g;
    g.expected = expectedRatio;
    // 第一遍：求 ROI 内 max|I2|
    double maxAbs2 = 0;
    for (int iy = 0; iy < i1.ny; ++iy)
        for (int ix = 0; ix < i1.nx; ++ix) {
            if (!roi.contains(i1.x(ix), i1.y(iy))) continue;
            maxAbs2 = std::max(maxAbs2, (double)std::fabs(i2.at(ix, iy)));
        }
    const double floor2 = relFloor * maxAbs2;
    double sum = 0, sum2 = 0; long n = 0;
    for (int iy = 0; iy < i1.ny; ++iy)
        for (int ix = 0; ix < i1.nx; ++ix) {
            const double xx = i1.x(ix), yy = i1.y(iy);
            if (!roi.contains(xx, yy)) continue;
            const double d = i2.at(ix, iy);
            if (std::fabs(d) < floor2) { ++g.nSkipped; continue; }
            const double r = i1.at(ix, iy) / d;
            sum += r; sum2 += r * r; ++n;
        }
    g.n = n;
    if (n == 0) { g.detail = "ROI 有效像素为空"; return g; }
    g.meanRatio = sum / n;
    const double var = std::max(0.0, sum2 / n - g.meanRatio * g.meanRatio);
    g.relStd = (std::fabs(g.meanRatio) > 0) ? std::sqrt(var) / std::fabs(g.meanRatio) : 0;
    g.relErrVsExpected = (std::fabs(expectedRatio) > 0)
        ? std::fabs(g.meanRatio - expectedRatio) / std::fabs(expectedRatio) : 0;
    g.ok = (g.relStd <= tolRelStd) && (g.relErrVsExpected <= tolRelErr);
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "n=%ld skipped=%ld meanRatio=%.6g relStd=%.3g (tol %.3g) "
                  "relErrVsExpected=%.3g (tol %.3g)",
                  n, g.nSkipped, g.meanRatio, g.relStd, tolRelStd,
                  g.relErrVsExpected, tolRelErr);
    g.detail = buf;
    return g;
}

}  // namespace pametric
