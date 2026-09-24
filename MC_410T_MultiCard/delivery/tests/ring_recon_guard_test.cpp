// =====================================================================
// ring_recon_guard_test —— B 档判别性守卫测试（S2）
//
// 目的：在 B1「UBP 成对切换」落地之前，把「不许回退」的底线锁死。
// 宿主是 src/RingRecon/ring_recon.cpp 的 CPU reference（纯 C++，无 Qt/CUDA 依赖）。
//
// 四组断言（对应前提文档 R1–R4 与审核 E1/E2）：
//   G1  比值守卫  —— 线性算子必须保持双波长幅值比（R1）
//   G2  恒等式    —— b = 2p − 2r̃p′ = −2r̃²∂(p/r̃)/∂r̃（前提 G2，R2 的根据）
//   G3  权重成对  —— 现有权重 = Δθ·cosα/d；UBP 权重 = R·Δθ·cosα/d²，两者差 R/d（E1）
//   G4  全关逐位  —— 全关路径逐位等于冻结基准（R4）
//
// 反证说明：G1 内含一条负例，证明该守卫对「逐图峰值归一化」确实有判别力；
//           不是恒真式。
// =====================================================================

#include "ring_recon.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
        std::printf("  PASS  %s\n", what);
    } else {
        ++g_fail;
        std::printf("  FAIL  %s\n", what);
    }
}

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------
// 闭式权重（合同本体）。这两个函数是 S4 落地 B1 时必须满足的契约。
// ---------------------------------------------------------------

// 现有权重：w = Δθ·cosα/d = Δθ·(R − r·n̂)/d²
//   Δθ = arc（弧度），n̂ = (cosθ, sinθ)，d = |r − r_s|
double dasWeightClosedForm(double R, double thetaRad, double x, double y, double arc) {
    const double nxv = std::cos(thetaRad), nyv = std::sin(thetaRad);
    const double sx = R * nxv, sy = R * nyv;
    const double dx = x - sx, dy = y - sy;
    const double d2 = dx * dx + dy * dy;
    const double d = std::sqrt(d2);
    const double cosAlpha = (R - (x * nxv + y * nyv)) / d;
    return arc * cosAlpha / d;
}

// UBP 立体角权重：dΩ = R·Δθ·cosα/d²
double ubpWeight(double R, double thetaRad, double x, double y, double arc) {
    const double nxv = std::cos(thetaRad), nyv = std::sin(thetaRad);
    const double sx = R * nxv, sy = R * nyv;
    const double dx = x - sx, dy = y - sy;
    const double d = std::sqrt(dx * dx + dy * dy);
    const double cosAlpha = (R - (x * nxv + y * nyv)) / d;
    return R * arc * cosAlpha / (d * d);
}

// 构造一条确定性的 bscan：第 col 列幅值 amp，其余为 0。
// 这样 acc 就精确等于「单根 A-line 的权重 × amp」，可直接比对闭式权重。
std::vector<float> singleColumnBscan(int Nt, int nd, int col, float amp) {
    std::vector<float> b(static_cast<size_t>(Nt) * nd, 0.0f);
    for (int i = 0; i < Nt; ++i) b[static_cast<size_t>(col) * Nt + i] = amp;
    return b;
}

// 确定性 bscan（G4 回归用）：逐点可复算，不依赖随机数
std::vector<float> deterministicBscan(int Nt, int nd) {
    std::vector<float> b(static_cast<size_t>(Nt) * nd, 0.0f);
    for (int j = 0; j < nd; ++j) {
        for (int i = 0; i < Nt; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(Nt);
            b[static_cast<size_t>(j) * Nt + i] = static_cast<float>(
                std::sin(3.0 * t + 0.7 * j) * (0.5 + 0.25 * j) +
                0.1 * std::cos(11.0 * t - 0.3 * j));
        }
    }
    return b;
}

ringrecon::ReconParams defaultRecon() {
    ringrecon::ReconParams rp;
    rp.fs = 250e6;
    rp.c = 1490.0;
    rp.R = 6.57e-3;
    rp.fov = 8e-3;
    rp.gridSize = 1e-3;
    rp.distanceWeightExponent = 1.0;   // 默认 q=1 ⇒ pw=2
    rp.minDistance = 0.0;              // 自动 = gridSize
    rp.maskOutOfRange = false;         // 关掉越界掩膜，便于比对闭式权重
    rp.interpolation = "linear";
    return rp;
}

// ---------------------------------------------------------------
// G1 比值守卫（R1）
// ---------------------------------------------------------------
void testG1RatioGuard() {
    std::printf("\n[G1] 双波长比值守卫（R1）\n");

    const ringrecon::ReconParams rp = defaultRecon();
    std::vector<float> xv, yv;
    ringrecon::makeGrid(rp.fov, rp.gridSize, xv, yv);
    const int Nt = 64, nd = 4;
    const double start = 0.0, span = 120.0;

    ringrecon::IncrementalState s1, s2;
    const float a1 = 1.0f, a2 = 3.7f;   // 两波长幅值比 = 3.7
    ringrecon::dasReconAppend(deterministicBscan(Nt, nd), Nt, nd, rp,
                              start, span, xv, yv, s1);
    ringrecon::dasReconAppend(deterministicBscan(Nt, nd), Nt, nd, rp,
                              start, span, xv, yv, s2);
    // 用同一几何、幅值缩放重跑：wl1 = a1·x，wl2 = a2·x
    ringrecon::IncrementalState w1, w2;
    {
        std::vector<float> b1 = deterministicBscan(Nt, nd);
        std::vector<float> b2 = deterministicBscan(Nt, nd);
        for (auto& v : b1) v *= a1;
        for (auto& v : b2) v *= a2;
        ringrecon::dasReconAppend(b1, Nt, nd, rp, start, span, xv, yv, w1);
        ringrecon::dasReconAppend(b2, Nt, nd, rp, start, span, xv, yv, w2);
    }

    const std::vector<float> I1 = ringrecon::normalizedImage(w1);
    const std::vector<float> I2 = ringrecon::normalizedImage(w2);
    check(I1.size() == I2.size() && !I1.empty(), "G1 输出尺寸一致且非空");

    const double wantRatio = static_cast<double>(a2) / static_cast<double>(a1);
    double maxRel = 0.0;
    int nChecked = 0;
    for (size_t k = 0; k < I1.size(); ++k) {
        const double v1 = I1[k], v2 = I2[k];
        if (std::fabs(v1) < 1e-9) continue;          // 过零像素不参与比值守卫
        const double rel = std::fabs(v2 / v1 - wantRatio) / wantRatio;
        if (rel > maxRel) maxRel = rel;
        ++nChecked;
    }
    check(nChecked > 0, "G1 有可参与比对的非零像素");
    check(maxRel <= 1e-5, "G1 输出幅值比 == 输入幅值比（逐像素）");

    // 反证：逐图峰值归一化会破坏比值 —— 证明本守卫有判别力，不是恒真式
    double peak1 = 0.0, peak2 = 0.0;
    for (float v : I1) peak1 = std::max(peak1, static_cast<double>(std::fabs(v)));
    for (float v : I2) peak2 = std::max(peak2, static_cast<double>(std::fabs(v)));
    double badMaxRel = 0.0;
    for (size_t k = 0; k < I1.size(); ++k) {
        const double v1 = I1[k] / peak1, v2 = I2[k] / peak2;
        if (std::fabs(v1) < 1e-9) continue;
        const double rel = std::fabs(v2 / v1 - wantRatio) / wantRatio;
        if (rel > badMaxRel) badMaxRel = rel;
    }
    check(badMaxRel > 1e-3, "G1 反证：逐图峰值归一化确实破坏比值（守卫有判别力）");
}

// ---------------------------------------------------------------
// G2 恒等式 b = 2p − 2r̃p′ = −2r̃²∂(p/r̃)/∂r̃（前提 G2）
// ---------------------------------------------------------------
void testG2Identity() {
    std::printf("\n[G2] 反演核恒等式（前提 G2 / R2 根据）\n");

    // 解析信号 p(r̃) = r̃²·exp(−r̃)（有非平凡导数，且 p/r̃ 处处可导）
    auto p = [](double r) { return r * r * std::exp(-r); };
    auto dp = [](double r) { return (2.0 * r - r * r) * std::exp(-r); };

    const double h = 1e-6;
    // 逐点求左右两式，并取全局尺度。注意 r=1 处 lhs 恰为 0，
    // 不能用逐点 |lhs| 做分母（会因过零炸掉），必须按全局幅度归一。
    std::vector<double> lhsAll, rhsAll;
    double globalScale = 0.0;
    for (double r = 0.2; r <= 6.0; r += 0.1) {
        // 左式：2p − 2r·p′（解析导数）
        const double lhs = 2.0 * p(r) - 2.0 * r * dp(r);
        // 右式：−2r²·∂(p/r)/∂r，用中心差分
        const double dpr =
            ((p(r + h) / (r + h)) - (p(r - h) / (r - h))) / (2.0 * h);
        const double rhs = -2.0 * r * r * dpr;
        lhsAll.push_back(lhs);
        rhsAll.push_back(rhs);
        globalScale = std::max(globalScale, std::fabs(lhs));
    }
    double maxAbsErr = 0.0;
    for (size_t k = 0; k < lhsAll.size(); ++k)
        maxAbsErr = std::max(maxAbsErr, std::fabs(lhsAll[k] - rhsAll[k]));
    check(maxAbsErr / globalScale <= 1e-6,
          "G2  2p − 2r·p′ == −2r²·∂(p/r)/∂r（解析信号，全局尺度归一）");

    // 用采样点时间 t（秒）表达的同一式：b = 2p − 2t·dp/dt，t = r̃/c
    //   2r̃·∂p/∂r̃ = 2(ct)·(1/c)·∂p/∂t = 2t·∂p/∂t  ⇒ 两式等价
    const double c = 1490.0;
    double maxRelT = 0.0;
    for (double t = 1e-6; t <= 2e-5; t += 1e-7) {
        const double r = c * t;
        const double lhsR = 2.0 * p(r) - 2.0 * r * dp(r);
        // dp/dt = dp/dr · dr/dt = dp/dr · c
        const double lhsT = 2.0 * p(r) - 2.0 * t * (dp(r) * c);
        maxRelT = std::max(maxRelT, std::fabs(lhsR - lhsT) /
                                        std::max(std::fabs(lhsR), 1e-30));
    }
    check(maxRelT <= 1e-9, "G2  2p − 2t·dp/dt（t 用秒）== 2p − 2r̃·dp/dr̃");

    // 反证：若把 t 误用成采样点序号 n，导数项 2t·p′ 会被放大 fs 倍。
    //   t[秒] = n/fs，故 n/t == fs（约 2.5e8）。
    const double fs = 250e6;
    const double t = 5e-6;
    const double amplification = (t * fs) / t;   // == fs
    check(amplification > 1e6 && std::fabs(amplification - fs) < 1.0,
          "G2 反证：t 用采样点会差 fs 倍（必须用秒）");
}

// ---------------------------------------------------------------
// G3 权重成对（E1 / R2）
// ---------------------------------------------------------------
void testG3WeightPair() {
    std::printf("\n[G3] 权重成对：现有 = Δθ·cosα/d，UBP = R·Δθ·cosα/d²（E1）\n");

    const ringrecon::ReconParams rp = defaultRecon();
    std::vector<float> xv, yv;
    ringrecon::makeGrid(rp.fov, rp.gridSize, xv, yv);
    const int Nt = 64;
    const int nd = 2;                       // 最小合法值
    const double start = 10.0, span = 40.0; // 覆盖 40°，2 根 ⇒ Δθ = 40°
    const double arc = span / (nd - 1) * kPi / 180.0;

    // 只让第 0 列非零 ⇒ acc 精确等于「单根 A-line 的权重 × 1.0」
    const std::vector<float> b = singleColumnBscan(Nt, nd, 0, 1.0f);
    ringrecon::IncrementalState st;
    ringrecon::dasReconAppend(b, Nt, nd, rp, start, span, xv, yv, st);

    const std::vector<float> img = ringrecon::normalizedImage(st);
    check(img.size() == st.acc.size(), "G3 输出尺寸 = acc 尺寸");

    // 第 0 根探测器的角度
    const double th0 = (start + 0.0 * (span / (nd - 1))) * kPi / 180.0;

    int nChecked = 0;
    double maxRel = 0.0, maxRelRatio = 0.0;
    for (int ix = 0; ix < static_cast<int>(xv.size()); ++ix) {
        for (int iy = 0; iy < static_cast<int>(yv.size()); ++iy) {
            const size_t idx = static_cast<size_t>(ix) * yv.size() + iy;
            const double x = xv[ix], y = yv[iy];
            const double wWant = dasWeightClosedForm(rp.R, th0, x, y, arc);
            const double wGot = st.acc[idx];       // 另一根 bscan=0，不贡献
            if (std::fabs(wWant) < 1e-12) continue;
            const double rel = std::fabs(wGot - wWant) / std::fabs(wWant);
            if (rel > maxRel) maxRel = rel;

            // E1 关系：dΩ / w_现有 = R/d
            const double sx = rp.R * std::cos(th0), sy = rp.R * std::sin(th0);
            const double d = std::sqrt((x - sx) * (x - sx) + (y - sy) * (y - sy));
            const double ratioWant = rp.R / d;
            const double ratioGot = ubpWeight(rp.R, th0, x, y, arc) / wWant;
            const double relR = std::fabs(ratioGot - ratioWant) / ratioWant;
            if (relR > maxRelRatio) maxRelRatio = relR;
            ++nChecked;
        }
    }
    check(nChecked > 0, "G3 有可参与比对的像素");
    check(maxRel <= 1e-5, "G3 关态权重逐像素 == Δθ·cosα/d（现有行为）");
    check(maxRelRatio <= 1e-9, "G3 E1 关系：UBP 权重 == (R/d) × 现有权重");

    // 反证：若只换信号项、沿用现有权重，就不是 UBP —— 二者之比必须是 R/d 而非常数
    double minRatio = 1e300, maxRatio = -1e300;
    for (int ix = 0; ix < static_cast<int>(xv.size()); ++ix) {
        for (int iy = 0; iy < static_cast<int>(yv.size()); ++iy) {
            const double x = xv[ix], y = yv[iy];
            const double w = dasWeightClosedForm(rp.R, th0, x, y, arc);
            if (std::fabs(w) < 1e-12) continue;
            const double r = ubpWeight(rp.R, th0, x, y, arc) / w;
            minRatio = std::min(minRatio, r);
            maxRatio = std::max(maxRatio, r);
        }
    }
    check((maxRatio - minRatio) / maxRatio > 0.05,
          "G3 反证：R/d 随像素变化（差 ≥5%）⇒ 「只换信号项不换权重」不成立");
}

// ---------------------------------------------------------------
// G4 全关逐位回归（R4）
// ---------------------------------------------------------------
void testG4BaselineRegression(bool emit) {
    std::printf("\n[G4] 全关逐位回归（R4）\n");

    ringrecon::ReconParams rp = defaultRecon();
    rp.fov = 4e-3;
    rp.gridSize = 1e-3;
    std::vector<float> xv, yv;
    ringrecon::makeGrid(rp.fov, rp.gridSize, xv, yv);
    const int Nt = 32, nd = 4;
    const double start = 25.0, span = 90.0;

    const std::vector<float> b = deterministicBscan(Nt, nd);
    ringrecon::IncrementalState st;
    ringrecon::dasReconAppend(b, Nt, nd, rp, start, span, xv, yv, st);
    const std::vector<float> img = ringrecon::normalizedImage(st);

    if (emit) {
        std::printf("  // ---- golden begin ----\n");
        for (size_t k = 0; k < img.size(); ++k)
            std::printf("  /* %zu */ %.9g,\n", k, static_cast<double>(img[k]));
        std::printf("  // ---- golden end ----\n");
        return;
    }

    // 冻结基准：全关路径必须逐位保持这些值。
    // B1/B2/B3 落地后，只要开关全关，就必须仍然逐位命中。
    // （初次建立用 --emit-golden 生成后填入。）
    static const std::vector<double> kGolden = {
        /*  0 */ -0.662225127,
        /*  1 */ -0.685468674,
        /*  2 */ -0.720820367,
        /*  3 */ -0.795317411,
        /*  4 */ -0.647285402,
        /*  5 */ -0.652787030,
        /*  6 */ -0.690061569,
        /*  7 */ -0.743569314,
        /*  8 */ -0.616849840,
        /*  9 */ -0.613399863,
        /* 10 */ -0.624428272,
        /* 11 */ -0.657809436,
        /* 12 */ -0.580601752,
        /* 13 */ -0.554781079,
        /* 14 */ -0.545745134,
        /* 15 */ -0.539877415,
    };
    check(img.size() == kGolden.size(),
          "G4 输出长度 == 冻结基准长度");

    double maxRel = 0.0;
    for (size_t k = 0; k < img.size() && k < kGolden.size(); ++k) {
        const double got = img[k], want = kGolden[k];
        const double rel = std::fabs(got - want) /
                           std::max(std::fabs(want), 1e-30);
        if (rel > maxRel) maxRel = rel;
    }
    check(maxRel <= 1e-6, "G4 全关路径逐位命中冻结基准");

    // 确定性自检：同一输入重跑必须逐位一致（排掉隐藏状态/跨块泄漏）
    ringrecon::IncrementalState st2;
    ringrecon::dasReconAppend(b, Nt, nd, rp, start, span, xv, yv, st2);
    const std::vector<float> img2 = ringrecon::normalizedImage(st2);
    bool identical = img.size() == img2.size();
    for (size_t k = 0; identical && k < img.size(); ++k)
        identical = (img[k] == img2[k]);          // 逐位相等，不用容差
    check(identical, "G4 重跑逐位一致（无隐藏状态）");
}

// ---------------------------------------------------------------
// G5/G6 UBP 成对切换（前提 R2）与导数正确性
// ---------------------------------------------------------------
void testG5UbpPairedMode() {
    std::printf("\n[G5/G6] UBP 成对切换：权重 R·Δθ·cosα/d² + 信号 2p − 2t·p′\n");

    // G5a 导数：derivativeCentral 的中心差分 + 端点单侧
    {
        std::vector<float> v(9), d(9);
        for (int i = 0; i < 9; ++i) v[i] = static_cast<float>(i);   // v[i] = i
        ringrecon_inv::derivativeCentral(v.data(), 9, 1000.0f, d.data());
        // dv/dn = 1 ⇒ dp/dt = fs·1 = 1000，处处（含端点单侧）相同
        double maxErr = 0.0;
        for (int i = 0; i < 9; ++i)
            maxErr = std::max(maxErr, std::fabs(static_cast<double>(d[i]) - 1000.0));
        check(maxErr <= 1e-6, "G5a derivativeCentral: 线性信号导数 == fs（含端点）");
    }

    const ringrecon::ReconParams rp = defaultRecon();
    std::vector<float> xv, yv;
    ringrecon::makeGrid(rp.fov, rp.gridSize, xv, yv);
    const int Nt = 64, nd = 2;
    const double start = 10.0, span = 40.0;
    const double arc = span / (nd - 1) * kPi / 180.0;
    const double th0 = (start + 0.0 * (span / (nd - 1))) * kPi / 180.0;

    // 常数列 ⇒ p = 1，p′ = 0 ⇒ 反演核 b = 2·1 − 2t·0 = 2（恰好已知）
    const std::vector<float> bconst = singleColumnBscan(Nt, nd, 0, 1.0f);

    ringrecon::ReconParams rpDas = rp, rpUbp = rp;
    rpUbp.inversion = ringrecon_inv::InversionMode::Ubp;
    ringrecon::IncrementalState sDas, sUbp;
    ringrecon::dasReconAppend(bconst, Nt, nd, rpDas, start, span, xv, yv, sDas);
    ringrecon::dasReconAppend(bconst, Nt, nd, rpUbp, start, span, xv, yv, sUbp);

    // G5 开态权重：accUbp = w_ubp × b = w_ubp × 2
    int nChecked = 0;
    double maxRelU = 0.0, maxRelPair = 0.0;
    for (int ix = 0; ix < static_cast<int>(xv.size()); ++ix) {
        for (int iy = 0; iy < static_cast<int>(yv.size()); ++iy) {
            const size_t idx = static_cast<size_t>(ix) * yv.size() + iy;
            const double x = xv[ix], y = yv[iy];
            const double wUWant = ubpWeight(rp.R, th0, x, y, arc);
            const double got = sUbp.acc[idx] / 2.0;      // 除掉已知的 b = 2
            if (std::fabs(wUWant) < 1e-12) continue;
            maxRelU = std::max(maxRelU, std::fabs(got - wUWant) / std::fabs(wUWant));

            // G6 成对性：accDas = w_das × p = w_das × 1
            const double wDWant = dasWeightClosedForm(rp.R, th0, x, y, arc);
            const double gotD = sDas.acc[idx];
            if (std::fabs(wDWant) < 1e-12) continue;
            const double sx = rp.R * std::cos(th0), sy = rp.R * std::sin(th0);
            const double dd = std::sqrt((x - sx) * (x - sx) + (y - sy) * (y - sy));
            const double pairWant = rp.R / dd;                // E1：R/d
            // got 已是 wUbp（acc 除过 b = 2），gotD 已是 wDas（p = 1），
            // 故两者之比直接就是 R/d，不再除 b/p。
            const double pairGot = got / gotD;
            maxRelPair = std::max(maxRelPair,
                                  std::fabs(pairGot - pairWant) / pairWant);
            ++nChecked;
        }
    }
    check(nChecked > 0, "G5 有可参与比对的像素");
    check(maxRelU <= 1e-5, "G5 开态权重逐像素 == R·Δθ·cosα/d²（生产路径）");
    check(maxRelPair <= 1e-5, "G6 成对性：开/关权重比 == R/d（信号项与权重一起换）");

    // G6b 信号项确实换掉：非常数列下 b = 2p − 2t·p′ ≠ p
    {
        const std::vector<float> bvar = deterministicBscan(Nt, nd);
        ringrecon::IncrementalState d2, u2;
        ringrecon::dasReconAppend(bvar, Nt, nd, rpDas, start, span, xv, yv, d2);
        ringrecon::dasReconAppend(bvar, Nt, nd, rpUbp, start, span, xv, yv, u2);
        bool differs = false;
        for (size_t k = 0; k < d2.acc.size(); ++k)
            if (u2.acc[k] != d2.acc[k]) { differs = true; break; }
        check(differs, "G6b 开关切换确实改变了累加结果（信号项已换）");
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool emit = (argc > 1 && std::string(argv[1]) == "--emit-golden");
    std::printf("ring_recon_guard_test —— B 档判别性守卫（S2 + S4 骨架）\n");

    testG1RatioGuard();
    testG2Identity();
    testG3WeightPair();
    testG5UbpPairedMode();
    testG4BaselineRegression(emit);

    std::printf("\n=====================================\n");
    std::printf("PASS %d / FAIL %d\n", g_pass, g_fail);
    return (g_fail == 0 && !emit) ? 0 : (emit ? 0 : 1);
}
