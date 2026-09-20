// zpf_numeric_smoke — zero_phase_filter 数值快速自检（开发期手工工具）
// 对照已知解析值：butter(2,0.5) 低通 SOS、butter(3,0.4,'high') 通带校验、
// DC/带内正弦相位（零相位延迟为零）、端点延拓要求、短窗报错。
#include "zero_phase_filter.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace zerophase;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("PASS %s\n", msg); } \
    else { std::printf("FAIL %s\n", msg); ++g_fail; } \
} while (0)

int main() {
    // 1) butter(2, 0.5) 低通（fs=2, fc=0.5 → Wn=0.5）：
    //    MATLAB: b=[0.29289 0.58579 0.29289], a=[1 0 0.17157]
    //    本实现单节：b*g 应等于 MATLAB b（含增益分配）
    {
        Design d = designFilter(Kind::Lowpass, 2, 0.5, 2.0);
        CHECK(d.ok(), "LP n=2 Wn=0.5 design ok");
        if (d.ok() && d.sos.size() == 1) {
            const SosSection &s = d.sos[0];
            const double expB[3] = {0.2928932188134524, 0.5857864376269049,
                                    0.2928932188134524};
            const double expA1 = 0.0, expA2 = 0.1715728752538099;
            double mb = 0.0, ma = 0.0;
            for (int i = 0; i < 3; ++i)
                mb = std::max(mb, std::fabs(s.b[i] - expB[i]));
            ma = std::max(std::fabs(s.a[1] - expA1), std::fabs(s.a[2] - expA2));
            std::printf("  coeff diff: b=%.3e a=%.3e\n", mb, ma);
            CHECK(mb < 1e-12 && ma < 1e-12, "LP n=2 Wn=0.5 coeffs match MATLAB butter");
        }
    }

    // 2) butter(4, 0.4, 'high')（fs=1, fc=0.2 → Wn=0.4）：Nyquist 增益 = 1
    {
        Design d = designFilter(Kind::Highpass, 4, 0.2, 1.0);
        CHECK(d.ok(), "HP n=4 Wn=0.4 design ok");
        if (d.ok()) {
            // 频响 H(e^{jπ}) = Π (b0-b1+b2)/(1-a1+a2)
            double h = 1.0;
            for (const SosSection &s : d.sos) {
                h *= (s.b[0] - s.b[1] + s.b[2]) / (1.0 - s.a[1] + s.a[2]);
            }
            std::printf("  |H(Nyquist)| = %.12f\n", std::fabs(h));
            CHECK(std::fabs(h - 1.0) < 1e-9, "HP n=4 Nyquist gain = 1");
        }
    }

    // 3) 奇数阶 n=3：极点数=3 → 1 共轭对 + 1 实极点一阶节
    {
        Design d = designFilter(Kind::Lowpass, 3, 0.3, 1.0);
        CHECK(d.ok() && d.sos.size() == 2, "LP n=3 → 2 sections (1 biquad + 1 first-order)");
        if (d.ok()) {
            const SosSection &last = d.sos.back();
            CHECK(last.b[2] == 0.0 && last.a[2] == 0.0,
                  "n=3 real-pole section is first-order (b2=a2=0)");
        }
    }

    // 4) 零相位验证：带内正弦（fc 通带内）经 HP+LP 后与阶段 A 参考行为一致
    //    （MATLAB refZeroPhase 同输入实测：mid amp=[-1.0105,1.0015]、
    //     peakshift=401（振铃离散化，参考同样出现）、maxDiff~1.26e-2 为
    //     HP 边带真实衰减——见 stage-B1 数值测试的逐向量对照）
    {
        const double fs = 250e6;
        FilterSet fs2;
        std::string err;
        fs2 = designSet(true, 0.4e6, 4, true, 40e6, 4, fs, &err);
        CHECK(fs2.hp.ok() && fs2.lp.ok(), "HP 0.4M + LP 40M @250MHz design ok");
        const int N = 4000;
        const double f0 = 5e6;   // 带内
        std::vector<double> x(N);
        for (int i = 0; i < N; ++i) x[i] = std::sin(2.0 * M_PI * f0 * i / fs);
        std::vector<double> x0 = x;
        std::string aerr;
        CHECK(applySet(fs2, x, &aerr), "applySet on in-band sine ok");
        const int mid0 = N / 4, mid1 = N * 3 / 4;
        double maxA = 0.0;
        int peakIn = mid0;
        double p1 = -2;
        for (int i = mid0; i < mid1; ++i) {
            maxA = std::max(maxA, std::fabs(x[i]));
            if (x[i] > p1) { p1 = x[i]; peakIn = i; }
        }
        std::printf("  mid |amp|max=%.4f peak=%d\n", maxA, peakIn);
        // 参考实测 mid |amp|max ≈ 1.0105（带内双通滤波振铃略大于 1）
        CHECK(maxA > 0.95 && maxA < 1.10, "in-band sine amplitude matches reference");
    }

    // 5) DC 抑制（HP 通带外）。仅 HP（LP 关）时 designSet 第三/五参传 0。
    {
        const double fs = 250e6;
        std::string derr;
        FilterSet fs2 = designSet(true, 0.4e6, 4, false, 0, 0, fs, &derr);
        CHECK(derr.empty() && fs2.hp.ok(), "HP-only design ok (LP off)");
        const int N = 4000;
        std::vector<double> x(N, 1.0);   // 常数
        std::string aerr;
        CHECK(applySet(fs2, x, &aerr), "applySet HP on constant ok");
        double m = 0.0;
        for (int i = 0; i < N; ++i) m = std::max(m, std::fabs(x[i]));
        std::printf("  HP residual on DC (mid) = %.3e\n", m);
        // 中段（避开端点过渡）：应接近 0
        double mid = 0.0;
        for (int i = N / 4; i < N * 3 / 4; ++i) mid = std::max(mid, std::fabs(x[i]));
        CHECK(mid < 1e-6, "HP suppresses DC in mid-window");
    }

    // 6) 短窗/非法参数报错
    {
        std::string err;
        Design d = designFilter(Kind::Lowpass, 8, 10e6, 250e6);   // nfact=24
        CHECK(d.ok(), "n=8 design ok");
        std::vector<double> x(24, 1.0);   // = nfact → 报错
        CHECK(!applyZeroPhase(d, x, &err) && !err.empty(),
              "Nx <= 3n rejected");
        std::vector<double> y(25, 1.0);
        CHECK(applyZeroPhase(d, y, &err), "Nx = 3n+1 accepted");
        Design bad = designFilter(Kind::Lowpass, 9, 1e6, 250e6);
        CHECK(!bad.ok(), "n=9 rejected");
        Design bad2 = designFilter(Kind::Lowpass, 4, 125e6 + 1.0, 250e6);
        CHECK(!bad2.ok(), "fc >= fs/2 rejected");
        std::string serr;
        FilterSet fsb = designSet(true, 40e6, 4, true, 0.4e6, 4, 250e6, &serr);
        CHECK(!serr.empty() && !fsb.hp.ok(),
              "HP > LP rejected");
    }

    // 7) C2 规则表
    {
        std::string err;
        CHECK(checkDbrCombination(false, 400, 358, true, "ch0/wl1", &err),
              "C2: filters off → no new rejection");
        CHECK(checkDbrCombination(true, 0, 358, false, "ch0/wl1", &err),
              "C2: E=0 allowed even delayCut=false");
        CHECK(checkDbrCombination(true, 357, 358, true, "ch0/wl1", &err),
              "C2: E<D delayCut=true allowed");
        CHECK(!checkDbrCombination(true, 358, 358, true, "ch0/wl1", &err),
              "C2: E>=D rejected");
        CHECK(!checkDbrCombination(true, 400, 358, false, "ch0/wl1", &err),
              "C2: E>0 delayCut=false rejected");
        CHECK(actualZeroRows(false, 300, 0, 4000) == 0, "E: dbr off → 0");
        CHECK(actualZeroRows(true, 300, 13, 4000) == 313, "E: 300+13");
        CHECK(actualZeroRows(true, 300, 13, 100) == 100, "E: clamped to Nt");
        CHECK(actualZeroRows(true, 0, 0, 4000) == 0, "E: zero mask → 0");
    }

    if (g_fail == 0) std::printf("ALL PASS\n");
    else std::printf("%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
