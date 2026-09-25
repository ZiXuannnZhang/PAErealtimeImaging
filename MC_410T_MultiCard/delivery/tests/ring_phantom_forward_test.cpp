// =====================================================================
// ring_phantom_forward_test —— 合成体模前向模型的契约测试
//
// 定位：前向模型是 **参考实现**（不调用任何生产/重建函数）；本测试把生产重建
//       （ringrecon::dasReconAppend）当**被测对象**，用独立前向模型喂已知真值数据。
//       这样得到的是真正的往返闭环，而不是「用生产代码验证生产代码」的自证。
//
// 波形选择的用意（P2/P6）：
//   * Delta（单极）用于**定位**断言 —— 峰值位置明确。
//   * GaussDeriv（N 形，奇函数）用于**反演核差异**断言。在完整环上，普通 DAS 会把
//     36 根 A-line 的反对称响应互相抵消，吸收体处接近 0；而 UBP 的
//     b = 2p − 2t·p′ 在该点给出 2τ/σ² ≠ 0。这正是光声反演要解决的问题，
//     也是 D7 要量的差异 —— 在这里先锁死现象确实存在。
// =====================================================================

#include "RingPhantomForward.h"
#include "ring_recon.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("  PASS  %s\n", what); }
    else    { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

constexpr double kPi = 3.14159265358979323846;

// 与项目默认值一致（ring_recon_cuda_set_defaults）
ringphantom::Geometry projectGeometry() {
    ringphantom::Geometry g;
    g.fs = 250e6;
    g.c = 1490.0;
    g.sampDepth = 4000;
    return g;
}

ringrecon::ReconParams projectRecon() {
    ringrecon::ReconParams rp;
    rp.fs = 250e6;
    rp.c = 1490.0;
    rp.R = 6.57e-3;
    rp.fov = 8e-3;
    rp.gridSize = 0.2e-3;
    rp.distanceWeightExponent = 1.0;
    rp.minDistance = 0.0;
    rp.maskOutOfRange = true;
    rp.interpolation = "linear";
    return rp;
}

// 36 根 A-line 均布（0°..350°，步进 10°），无重复端点
void projectAngles(std::vector<double>& thetaRad, std::vector<double>& radii,
                   double R, int nd) {
    thetaRad.resize(nd);
    radii.resize(nd);
    for (int j = 0; j < nd; ++j) {
        thetaRad[(std::size_t)j] = (10.0 * j) * kPi / 180.0;
        radii[(std::size_t)j] = R;
    }
}

// 找 |img| 最大处的像素下标
void peakOf(const std::vector<float>& img, std::size_t ny, int& ix, int& iy) {
    std::size_t best = 0;
    double bestAbs = -1.0;
    for (std::size_t k = 0; k < img.size(); ++k) {
        const double a = std::fabs((double)img[k]);
        if (a > bestAbs) { bestAbs = a; best = k; }
    }
    ix = (int)(best / ny);
    iy = (int)(best % ny);
}

// ---------------------------------------------------------------- P1
void testP1PulseShape() {
    std::printf("\n[P1] 脉冲波形\n");
    const double sigma = 4e-9;
    const double before = ringphantom::pulseValue(ringphantom::PulseShape::GaussDeriv,
                                                  -2.0 * sigma, sigma);
    const double after  = ringphantom::pulseValue(ringphantom::PulseShape::GaussDeriv,
                                                  +2.0 * sigma, sigma);
    check(before > 0.0 && after < 0.0, "P1 GaussDeriv 是 N 形（正后随负）");
    check(std::fabs(before + after) < 1e-6 * std::fabs(before),
          "P1 GaussDeriv 关于 t=τ 反对称（g(0)=0）");
    check(ringphantom::pulseValue(ringphantom::PulseShape::GaussDeriv, 0.0, sigma) == 0.0,
          "P1 GaussDeriv 在 t=τ 处恰为 0");

    double area = 0.0;
    const int n = 20001;
    const double lo = -10.0 * sigma, hi = 10.0 * sigma;
    const double h = (hi - lo) / (n - 1);
    for (int i = 0; i < n; ++i)
        area += ringphantom::pulseValue(ringphantom::PulseShape::Delta,
                                        lo + i * h, sigma);
    area *= h;
    check(std::fabs(area - 1.0) < 1e-3, "P1 Delta 近似面积 == 1");
}

// ---------------------------------------------------------------- P2
// 单极（Delta）脉冲：往返定位必须准确 —— 这是前向模型几何/走时正确性的判据
void testP2RoundTripPosition() {
    std::printf("\n[P2] 已知真值往返（Delta 脉冲，生产重建作被测对象）\n");

    const ringphantom::Geometry geo = projectGeometry();
    const ringrecon::ReconParams rp = projectRecon();
    const int nd = 36;
    std::vector<double> thetaRad, radii;
    projectAngles(thetaRad, radii, rp.R, nd);
    std::vector<float> xv, yv;
    ringrecon::makeGrid(rp.fov, rp.gridSize, xv, yv);

    // 真值直接取自网格点 ⇒ 位置误差理想值是 0
    const int ixTrue = 12, iyTrue = 20;
    ringphantom::Source s;
    s.x = xv[(std::size_t)ixTrue];
    s.y = yv[(std::size_t)iyTrue];
    s.amplitude = 1.0;

    ringphantom::ForwardParams fp;
    fp.pulse = ringphantom::PulseShape::Delta;

    const std::vector<float> bscan =
        ringphantom::simulate({s}, thetaRad, radii, geo, fp);
    check(bscan.size() == (std::size_t)geo.sampDepth * nd, "P2 bscan 尺寸 = Nt×nd");

    ringrecon::IncrementalState st;
    ringrecon::dasReconAppend(bscan, geo.sampDepth, nd, rp, 0.0, 350.0, xv, yv, st);
    const std::vector<float> img = ringrecon::normalizedImage(st);
    check(img.size() == xv.size() * yv.size(), "P2 图像尺寸 = 网格点数");

    int ix = 0, iy = 0;
    peakOf(img, yv.size(), ix, iy);
    const double dx = xv[(std::size_t)ix] - s.x;
    const double dy = yv[(std::size_t)iy] - s.y;
    const double err = std::sqrt(dx * dx + dy * dy);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "P2 峰值位置误差 %.4f mm（网格步长 %.4f mm）",
                  err * 1e3, rp.gridSize * 1e3);
    check(err <= rp.gridSize * 1.001, buf);

    // 前向幅值线性（R1 的前置：前向必须线性，比值守卫才谈得上）
    ringphantom::Source s2 = s;
    s2.amplitude = 3.7;
    const std::vector<float> bscan2 =
        ringphantom::simulate({s2}, thetaRad, radii, geo, fp);
    ringrecon::IncrementalState st2;
    ringrecon::dasReconAppend(bscan2, geo.sampDepth, nd, rp, 0.0, 350.0, xv, yv, st2);
    const std::vector<float> img2 = ringrecon::normalizedImage(st2);
    double maxRel = 0.0;
    int nChecked = 0;
    for (std::size_t k = 0; k < img.size(); ++k) {
        if (std::fabs((double)img[k]) < 1e-9) continue;
        const double rel = std::fabs(img2[k] / img[k] - 3.7) / 3.7;
        if (rel > maxRel) maxRel = rel;
        ++nChecked;
    }
    check(nChecked > 0, "P2 有可比对像素");
    check(maxRel <= 1e-5, "P2 前向幅值线性：3.7 倍源 ⇒ 3.7 倍重建（逐像素）");
}

// ---------------------------------------------------------------- P3
void testP3TwoSources() {
    std::printf("\n[P3] 双目标已知真值（Delta 脉冲）\n");

    const ringphantom::Geometry geo = projectGeometry();
    const ringrecon::ReconParams rp = projectRecon();
    const int nd = 36;
    std::vector<double> thetaRad, radii;
    projectAngles(thetaRad, radii, rp.R, nd);
    std::vector<float> xv, yv;
    ringrecon::makeGrid(rp.fov, rp.gridSize, xv, yv);

    ringphantom::Source a, b;
    a.x = xv[8];   a.y = yv[10];  a.amplitude = 1.0;
    b.x = xv[30];  b.y = yv[26];  b.amplitude = 1.0;

    ringphantom::ForwardParams fp;
    fp.pulse = ringphantom::PulseShape::Delta;

    const std::vector<float> bscan =
        ringphantom::simulate({a, b}, thetaRad, radii, geo, fp);
    ringrecon::IncrementalState st;
    ringrecon::dasReconAppend(bscan, geo.sampDepth, nd, rp, 0.0, 350.0, xv, yv, st);
    const std::vector<float> img = ringrecon::normalizedImage(st);

    auto localMax = [&](int ix, int iy) {
        const double v = std::fabs((double)img[(std::size_t)ix * yv.size() + iy]);
        for (int ddx = -2; ddx <= 2; ++ddx)
            for (int ddy = -2; ddy <= 2; ++ddy) {
                if (ddx == 0 && ddy == 0) continue;
                const int xx = ix + ddx, yy = iy + ddy;
                if (xx < 0 || yy < 0 || xx >= (int)xv.size() || yy >= (int)yv.size()) continue;
                if (std::fabs((double)img[(std::size_t)xx * yv.size() + yy]) > v) return false;
            }
        return true;
    };
    check(localMax(8, 10), "P3 真值 A 处为局部极大");
    check(localMax(30, 26), "P3 真值 B 处为局部极大");

    int ix = 0, iy = 0;
    peakOf(img, yv.size(), ix, iy);
    const double eA = std::sqrt((xv[ix] - a.x) * (xv[ix] - a.x) +
                                (yv[iy] - a.y) * (yv[iy] - a.y));
    const double eB = std::sqrt((xv[ix] - b.x) * (xv[ix] - b.x) +
                                (yv[iy] - b.y) * (yv[iy] - b.y));
    check(std::min(eA, eB) <= rp.gridSize * 1.001, "P3 最强峰落在某个真值位置");
}

// ---------------------------------------------------------------- P4
void testP4UniformDisk() {
    std::printf("\n[P4] 均匀盘体模\n");
    const std::vector<ringphantom::Source> disk =
        ringphantom::uniformDisk(1.0e-3, -2.0e-3, 1.5e-3, 2.0, 5);
    check(!disk.empty(), "P4 盘体模非空");
    check(disk.front().x == 1.0e-3 && disk.front().y == -2.0e-3, "P4 首点为盘心");
    bool inside = true;
    for (const auto& s : disk) {
        const double dx = s.x - 1.0e-3, dy = s.y + 2.0e-3;
        if (std::sqrt(dx * dx + dy * dy) > 1.5e-3 * 1.0000001) { inside = false; break; }
        if (s.amplitude != 2.0) { inside = false; break; }
    }
    check(inside, "P4 所有点在盘内且幅值一致");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "P4 点数随 nPointsPerRadius 增长（%zu 点）", disk.size());
    check(disk.size() > 30, buf);
}

// ---------------------------------------------------------------- P5
void testP5Contracts() {
    std::printf("\n[P5] 布局与输入契约\n");
    const ringphantom::Geometry geo = projectGeometry();
    ringphantom::ForwardParams fp;

    std::vector<double> th = {0.0, 1.0};
    std::vector<double> rad = {6.57e-3};
    check(ringphantom::simulate({ringphantom::Source{}}, th, rad, geo, fp).empty(),
          "P5 半径数与 A-line 数不匹配时返回空（不崩）");

    std::vector<double> rad2 = {6.57e-3, 6.57e-3};
    ringphantom::Geometry bad = geo; bad.fs = 0.0;
    check(ringphantom::simulate({ringphantom::Source{}}, th, rad2, bad, fp).empty(),
          "P5 非法采样率返回空（不崩）");

    ringphantom::Geometry tiny = geo; tiny.sampDepth = 0;
    check(ringphantom::simulate({ringphantom::Source{}}, th, rad2, tiny, fp).empty(),
          "P5 零采样深度返回空（不崩）");

    // 列主序：单探测器、单源，记录长度必须覆盖走时（τ = d/c ≈ 1102 样本）
    std::vector<double> th1 = {0.0}, rad1 = {6.57e-3};
    ringphantom::Geometry g1 = geo;
    g1.sampDepth = 4000;                       // 覆盖 τ
    const auto b1 = ringphantom::simulate({ringphantom::Source{0, 0, 1.0}},
                                          th1, rad1, g1, fp);
    check(b1.size() == 4000, "P5 单探测器输出长度 = Nt");
    int nonZero = 0;
    for (float v : b1) if (v != 0.0f) ++nonZero;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "P5 单探测器输出含非零样本（%d 个）", nonZero);
    check(nonZero > 0, buf);

    // 走时可复算：τ = d/c ⇒ 样本 = d/c·fs
    const double expectTau = 6.57e-3 / 1490.0 * 250e6;
    int peakI = 0;
    double peakV = -1.0;
    for (int i = 0; i < 4000; ++i) {
        const double a = std::fabs((double)b1[(std::size_t)i]);
        if (a > peakV) { peakV = a; peakI = i; }
    }
    std::snprintf(buf, sizeof(buf),
                  "P5 走时样本 %.1f == d/c·fs %.1f（±2 样本）",
                  (double)peakI, expectTau);
    check(std::fabs((double)peakI - expectTau) <= 2.0, buf);
}

// ---------------------------------------------------------------- P6
// N 形脉冲下 DAS 与 UBP 的差异 —— 这正是 D7 要量的东西，先锁死现象存在
void testP6BipolarDivergence() {
    std::printf("\n[P6] N 形脉冲：DAS 抵消 vs UBP 反演核（D7 现象前置）\n");

    const ringphantom::Geometry geo = projectGeometry();
    ringrecon::ReconParams rpDas = projectRecon(), rpUbp = projectRecon();
    rpUbp.inversion = ringrecon_inv::InversionMode::Ubp;
    const int nd = 36;
    std::vector<double> thetaRad, radii;
    projectAngles(thetaRad, radii, rpDas.R, nd);
    std::vector<float> xv, yv;
    ringrecon::makeGrid(rpDas.fov, rpDas.gridSize, xv, yv);

    const int ixTrue = 12, iyTrue = 20;
    ringphantom::Source s;
    s.x = xv[(std::size_t)ixTrue];
    s.y = yv[(std::size_t)iyTrue];
    s.amplitude = 1.0;

    ringphantom::ForwardParams fp;
    fp.pulse = ringphantom::PulseShape::GaussDeriv;   // N 形

    const std::vector<float> bscan =
        ringphantom::simulate({s}, thetaRad, radii, geo, fp);
    ringrecon::IncrementalState d, u;
    ringrecon::dasReconAppend(bscan, geo.sampDepth, nd, rpDas, 0.0, 350.0, xv, yv, d);
    ringrecon::dasReconAppend(bscan, geo.sampDepth, nd, rpUbp, 0.0, 350.0, xv, yv, u);
    const std::vector<float> imgD = ringrecon::normalizedImage(d);
    const std::vector<float> imgU = ringrecon::normalizedImage(u);

    const std::size_t atTrue = (std::size_t)ixTrue * yv.size() + iyTrue;
    double peakD = 0.0, peakU = 0.0;
    for (float v : imgD) peakD = std::max(peakD, (double)std::fabs(v));
    for (float v : imgU) peakU = std::max(peakU, (double)std::fabs(v));

    // 现象：N 形在完整环上被 DAS 互相抵消 ⇒ 真值处相对峰值极小
    const double relD = std::fabs((double)imgD[atTrue]) / (peakD > 0 ? peakD : 1.0);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "P6 真值处 DAS 相对幅度 %.3g（N 形在完整环上被抵消）", relD);
    check(relD < 0.25, buf);

    // UBP 反演核在 t=τ 处给 2τ/σ² ≠ 0，故真值处不再被抵消
    const double relU = std::fabs((double)imgU[atTrue]) / (peakU > 0 ? peakU : 1.0);
    std::snprintf(buf, sizeof(buf),
                  "P6 真值处 UBP 相对幅度 %.3g（反演核补偿了 N 形抵消）", relU);
    check(relU > relD * 2.0, buf);

    std::snprintf(buf, sizeof(buf), "P6 DAS/UBP 输出确实不同（峰值 %.4g vs %.4g）",
                  peakD, peakU);
    check(peakD != peakU, buf);
}

}  // namespace

int main() {
    std::printf("ring_phantom_forward_test —— 合成体模前向模型契约\n");
    testP1PulseShape();
    testP2RoundTripPosition();
    testP3TwoSources();
    testP4UniformDisk();
    testP5Contracts();
    testP6BipolarDivergence();
    std::printf("\n=====================================\n");
    std::printf("PASS %d / FAIL %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
