// d4_metrics_selftest.cpp — D4 单元自检
//
// 正例：解析构造的已知宽度高斯峰 + 已知幅值比的双波长图，断言各指标算得对。
// 负例：把两波长各自峰值归一化后，双波长比值守卫必须**失败**（证明判别力）。
//
// 编译（无 Qt/CUDA 依赖）：
//   g++ -std=c++17 -O2 -o d4_metrics_selftest d4_metrics_selftest.cpp
#include "pa_metrics.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("  PASS  %s\n", what); }
    else    { ++g_fail; std::printf("  FAIL  %s\n", what); }
}
static bool near(double a, double b, double relTol) {
    const double d = std::fabs(a - b);
    const double s = std::max(std::fabs(a), std::fabs(b));
    return s > 0 ? (d / s <= relTol) : (d <= relTol);
}

int main() {
    std::printf("=== D4 图像质量度量工具 自检 ===\n\n");

    // ---------- 网格 ----------
    const int n = 201;
    const double fov = 20e-3;            // 20 mm
    const double dx = fov / (n - 1);
    const double x0 = -fov / 2;
    std::vector<float> img1(n * n), img2(n * n);

    // 已知真值：高斯峰，σx = 0.30 mm（径向/轴向），σy = 0.60 mm（切向）
    const double sigX = 0.30e-3, sigY = 0.60e-3;
    const double peakX = 3.0e-3, peakY = 1.0e-3;
    const double A1 = 2.5;               // wl1 幅值
    const double AMPLITUDE_RATIO = 0.4;  // wl2 = 0.4 * wl1
    for (int iy = 0; iy < n; ++iy) {
        for (int ix = 0; ix < n; ++ix) {
            const double x = x0 + ix * dx, y = x0 + iy * dx;
            const double g = std::exp(-((x - peakX) * (x - peakX) / (2 * sigX * sigX) +
                                        (y - peakY) * (y - peakY) / (2 * sigY * sigY)));
            img1[iy * n + ix] = (float)(A1 * g);
            img2[iy * n + ix] = (float)(A1 * AMPLITUDE_RATIO * g);
        }
    }

    pametric::Image I1{ img1.data(), n, n, x0, x0, dx, dx };
    pametric::Image I2{ img2.data(), n, n, x0, x0, dx, dx };

    std::printf("[1] 峰值与位置误差\n");
    const pametric::Peak pk = pametric::findPeak(I1);
    std::printf("    峰值 (%.6f, %.6f) m, value=%.6f\n", pk.x, pk.y, pk.value);
    const double perr = pametric::positionErrorMm(pk, peakX, peakY);
    std::printf("    位置误差 = %.6f mm\n", perr);
    check(pk.found, "找到峰值");
    check(near(pk.value, A1, 1e-5), "峰值幅度 == A1");
    check(perr < dx * 1e3 * 1.5, "位置误差 < 1.5 个网格步长");

    std::printf("\n[2] 轴向/切向分辨率（-6 dB 全宽，径向/切向）\n");
    // 环心在 (0,0)；目标在 (3,1) mm，故径向≈(0.949,0.316)
    const pametric::Resolution res = pametric::resolution(I1, pk.x, pk.y, 0.0, 0.0);
    std::printf("    轴向(径向) -6dB 全宽 = %.6f mm\n", res.axialMm);
    std::printf("    切向       -6dB 全宽 = %.6f mm\n", res.tangentialMm);
    // 解析：高斯 |v| 降到峰值一半的全宽 = 2*σ*sqrt(2 ln2) = 2.3548σ
    const double expectedHalfWidth = 2.0 * std::sqrt(2.0 * std::log(2.0));
    // 注意目标不在坐标轴上，径向/切向是旋转后的轴；σx/σy 沿坐标轴。
    // 这里只断言切向宽度 > 轴向宽度（σy > σx）与量级正确。
    check(res.axialMm > 0 && res.tangentialMm > 0, "两个方向都求到 -6dB 宽度");
    check(res.tangentialMm > res.axialMm, "切向宽度 > 轴向宽度（σy>σx）");
    // 轴向沿 (0.949,0.316)：有效 σ = 1/sqrt((cos²/σx²)+(sin²/σy²))
    {
        const double cx = 3.0 / std::hypot(3.0, 1.0), cy = 1.0 / std::hypot(3.0, 1.0);
        const double inv2 = (cx * cx) / (sigX * sigX) + (cy * cy) / (sigY * sigY);
        const double sigEff = 1.0 / std::sqrt(inv2);
        const double expAx = expectedHalfWidth * sigEff * 1e3;
        std::printf("    解析期望 轴向宽度 = %.6f mm\n", expAx);
        check(near(res.axialMm, expAx, 0.08), "轴向宽度与解析值一致（8% 内）");
    }

    std::printf("\n[3] CR / 背景噪声 / CNR\n");
    pametric::Roi bg{ 0.0, -6.0e-3, 3.0e-3, 2.0e-3 };    // 背景：远离目标、不含目标
    pametric::Roi tgt{ peakX, peakY, 1.5 * sigX, 1.5 * sigY };
    std::printf("    背景 ROI %s\n", bg.describe().c_str());
    std::printf("    目标 ROI %s\n", tgt.describe().c_str());
    const double cr = pametric::contrastDb(I1, bg);
    const pametric::BgNoise bn = pametric::backgroundNoise(I1, bg);
    const double c = pametric::cnr(I1, tgt, bg);
    std::printf("    CR = %.4f dB   背景 std = %.6g   相对峰值 = %.6g   CNR = %.4f\n",
                cr, bn.sd, bn.relativeToPeak, c);
    // 高斯在 y=-6mm 处：exp(-(( -6-1)^2)/(2*0.6^2)) = exp(-68) ≈ 0 ⇒ 背景≈0
    check(std::fabs(cr) > 100.0 || bn.sd < 1e-6, "解析背景接近 0，CR 极大（口径自洽）");
    check(c > 0, "CNR 为正");

    std::printf("\n[4] gCNR（Kempski 2020）\n");
    const pametric::GCNRResult gr = pametric::gcnr(I1, tgt, bg, 64);
    std::printf("    gCNR = %.6f  (bins=%d, 灰度范围 [%.6g, %.6g])\n",
                gr.gcnr, gr.nbins, gr.lo, gr.hi);
    std::printf("    口径：pdf_T/pdf_B 为 ROI 归一化灰度直方图，灰度按两 ROI 合并 [min,max] 线性映射\n");
    check(gr.gcnr >= 0.0 && gr.gcnr <= 1.0, "gCNR 落在 [0,1]");
    check(gr.gcnr > 0.9, "完全分离的两 ROI 得 gCNR > 0.9");

    // ---------- 必选附加项：双波长比值守卫 ----------
    std::printf("\n[5] 双波长比值守卫（正例）\n");
    pametric::Roi roiAll{ 0.0, 0.0, 10e-3, 10e-3 };
    std::printf("    守卫 ROI %s\n", roiAll.describe().c_str());
    const double expectedRatio = 1.0 / AMPLITUDE_RATIO;  // I1/I2 = 1/0.4 = 2.5
    auto g1 = pametric::dualWavelengthRatioGuard(I1, I2, roiAll, expectedRatio);
    std::printf("    %s\n", g1.detail.c_str());
    std::printf("    期望 I1/I2 = %.6f（= 输入幅值比）\n", expectedRatio);
    check(g1.ok, "正例：比值守卫通过（常数比 = 输入幅值比）");
    check(near(g1.meanRatio, expectedRatio, 1e-4), "平均比值 == 输入幅值比");
    check(g1.relStd < 1e-4, "比值相对标准差 ~ 0（线性算子保持比值）");

    std::printf("\n[6] 双波长比值守卫（负例：逐图峰值归一化后必须失败）\n");
    // 这是判别性测试：把两幅图**各自**按自身峰值归一化，比值守卫必须 FAIL。
    std::vector<float> n1(img1.size()), n2(img2.size());
    const float p1 = std::fabs(pk.value);
    const pametric::Peak pk2 = pametric::findPeak(I2);
    const float p2 = std::fabs(pk2.value);
    for (size_t i = 0; i < img1.size(); ++i) { n1[i] = img1[i] / p1; n2[i] = img2[i] / p2; }
    pametric::Image N1{ n1.data(), n, n, x0, x0, dx, dx };
    pametric::Image N2{ n2.data(), n, n, x0, x0, dx, dx };
    auto g2 = pametric::dualWavelengthRatioGuard(N1, N2, roiAll, expectedRatio);
    std::printf("    归一化后 %s\n", g2.detail.c_str());
    std::printf("    （逐图峰值归一化后 I1/I2 恒为 1，而真值为 %.4f）\n", expectedRatio);
    check(!g2.ok, "负例：逐图峰值归一化使比值守卫【失败】（判别力成立）");
    check(g2.relErrVsExpected > 0.5, "负例：相对真值偏差 > 50%（确实被破坏）");

    std::printf("\n[7] 负例：硬整流（负值置零）后必须失败\n");
    {
        // 真实情形下两波长图是**近似**成比例（含噪声差异）而非严格成比例。
        // 硬整流是非线性：在过零点附近，噪声把一侧推过 0 而另一侧没有，
        // 比值会发散。构造 I2 = k*I1 + 小噪声来演示。
        std::vector<float> b1(img1.size()), b2(img1.size());
        const double k = AMPLITUDE_RATIO;
        for (size_t i = 0; i < img1.size(); ++i) {
            const double x = x0 + (int)(i % n) * dx;
            const double y = x0 + (int)(i / n) * dx;
            // 双极基底 + 已知幅值比 + 确定性小噪声
            const double base = std::exp(-((x - peakX) * (x - peakX) / (2 * sigX * sigX) +
                                           (y - peakY) * (y - peakY) / (2 * sigY * sigY))) - 0.5;
            const double noise = 0.02 * std::sin(37.0 * x / 1e-3) * std::cos(23.0 * y / 1e-3);
            b1[i] = (float)(base + noise);
            b2[i] = (float)(k * base + noise);   // 同一小噪声，故两图不严格成比例
        }
        // 口径说明：严格成比例（I2 = k*I1）时，整流后存活像素上的比值仍是 1/k，
        // 故本例必须带一点不严格成比例的差异（真实数据的两波长总带独立噪声）。
        // 整流的破坏发生在过零点附近：噪声把一侧推过 0 而另一侧没有，比值发散。
        pametric::Image B1{ b1.data(), n, n, x0, x0, dx, dx };
        pametric::Image B2{ b2.data(), n, n, x0, x0, dx, dx };
        auto gOk = pametric::dualWavelengthRatioGuard(B1, B2, roiAll, expectedRatio, 0.12, 0.12);
        std::printf("    未整流 %s\n", gOk.detail.c_str());
        check(gOk.ok, "未整流时比值守卫通过（近似成比例）");
        for (size_t i = 0; i < b1.size(); ++i) { if (b1[i] < 0) b1[i] = 0; if (b2[i] < 0) b2[i] = 0; }
        auto gR = pametric::dualWavelengthRatioGuard(B1, B2, roiAll, expectedRatio, 0.12, 0.12);
        std::printf("    硬整流后 %s\n", gR.detail.c_str());
        check(!gR.ok, "负例：硬整流使比值守卫【失败】（判别力成立）");
    }

    std::printf("\n[8] 负例：一侧做 Hilbert 取模（奇非线性）后必须失败\n");
    {
        // 严格成比例的双极对：I1 = s, I2 = k*s。
        // 一侧取模后比值 = |s|/(k*s) = +1/k（s>0）或 -1/k（s<0），不再是常数。
        std::vector<float> h1(img1.size()), h2(img1.size());
        const double k = AMPLITUDE_RATIO;
        for (size_t i = 0; i < img1.size(); ++i) {
            const double x = x0 + (int)(i % n) * dx;
            const double y = x0 + (int)(i / n) * dx;
            const double s = std::exp(-((x - peakX) * (x - peakX) / (2 * sigX * sigX) +
                                        (y - peakY) * (y - peakY) / (2 * sigY * sigY))) - 0.5;
            h1[i] = (float)std::fabs(s);   // 非线性（取模）
            h2[i] = (float)(k * s);        // 线性
        }
        pametric::Image H1{ h1.data(), n, n, x0, x0, dx, dx };
        pametric::Image H2{ h2.data(), n, n, x0, x0, dx, dx };
        auto g = pametric::dualWavelengthRatioGuard(H1, H2, roiAll, expectedRatio, 0.12, 0.12);
        std::printf("    %s\n", g.detail.c_str());
        check(!g.ok, "负例：一侧取模后比值守卫【失败】（判别力成立）");
    }

    std::printf("\n====================================\n");
    std::printf("PASS %d / FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
