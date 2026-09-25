#include "RingPhantomForward.h"

#include <algorithm>
#include <cmath>

// 参考实现。**刻意不 include 任何生产头**（ring_recon.h / RingReconInversion.h），
// 走时与几何全部自行实现，以免 D6/D7 变成自证。改动本文件前请保持这一约束。

namespace ringphantom {

namespace {
constexpr double kPi = 3.14159265358979323846;
// 脉冲支撑裁剪：|u| > kPulseCut·σ 外的贡献可忽略
constexpr double kPulseCut = 8.0;
}  // namespace

double pulseValue(PulseShape shape, double u, double sigma) {
    const double s = (sigma > 0.0) ? sigma : 1e-30;
    const double e = std::exp(-u * u / (2.0 * s * s));
    switch (shape) {
    case PulseShape::Delta:
        // 冲激的高斯近似（面积 1）：1/(σ√(2π))·exp(−u²/2σ²)
        return e / (s * std::sqrt(2.0 * kPi));
    case PulseShape::GaussDeriv:
        // 高斯一阶导：u<0 为正、u>0 为负 ⇒ **N 形（正后随负）**，符合实测 PA 波形
        return -(u / (s * s)) * e;
    case PulseShape::Ricker:
        return (1.0 - u * u / (s * s)) * e;
    }
    return 0.0;
}

std::vector<float> simulate(const std::vector<Source>& sources,
                            const std::vector<double>& thetaRad,
                            const std::vector<double>& radii,
                            const Geometry& geo,
                            const ForwardParams& fp) {
    const int nt = geo.sampDepth;
    const int nd = static_cast<int>(thetaRad.size());
    std::vector<float> bscan;
    if (nt <= 0 || nd <= 0) return bscan;
    if (static_cast<int>(radii.size()) != nd) return bscan;   // 半径数必须与 A-line 数一致
    if (!(geo.fs > 0.0) || !(geo.c > 0.0)) return bscan;

    bscan.assign(static_cast<std::size_t>(nt) * nd, 0.0f);

    const double dt = 1.0 / geo.fs;
    const double cut = kPulseCut * fp.pulseWidthSec;

    for (int j = 0; j < nd; ++j) {
        const double Rj = radii[static_cast<std::size_t>(j)];
        const double cth = std::cos(thetaRad[static_cast<std::size_t>(j)]);
        const double sth = std::sin(thetaRad[static_cast<std::size_t>(j)]);
        const double x0 = Rj * cth;
        const double y0 = Rj * sth;
        float* col = bscan.data() + static_cast<std::size_t>(j) * nt;

        for (std::size_t si = 0; si < sources.size(); ++si) {
            const Source& s = sources[si];
            const double dx = s.x - x0;
            const double dy = s.y - y0;
            const double d2 = dx * dx + dy * dy;
            const double d = std::sqrt(d2);
            const double dsafe = (d > 1e-30) ? d : 1e-30;

            const double tau = d / geo.c + fp.startDelaySec;

            double gain = s.amplitude;
            if (fp.spreadExponent != 0.0)
                gain *= std::pow(fp.referenceDistance / dsafe, fp.spreadExponent);
            if (fp.directivity) {
                const double cosAlpha = (Rj - (s.x * cth + s.y * sth)) / dsafe;
                gain *= std::max(0.0, cosAlpha);
            }
            if (gain == 0.0) continue;

            // 只在脉冲支撑 [τ − cut, τ + cut] 内累加
            int i0 = static_cast<int>(std::floor((tau - cut) * geo.fs));
            int i1 = static_cast<int>(std::ceil((tau + cut) * geo.fs));
            i0 = std::max(i0, 0);
            i1 = std::min(i1, nt - 1);
            for (int i = i0; i <= i1; ++i) {
                const double u = static_cast<double>(i) * dt - tau;
                col[i] += static_cast<float>(gain * pulseValue(fp.pulse, u, fp.pulseWidthSec));
            }
        }
    }
    return bscan;
}

std::vector<Source> uniformDisk(double centerX, double centerY, double radius,
                                double amplitude, int nPointsPerRadius) {
    std::vector<Source> out;
    const int rings = std::max(1, nPointsPerRadius);
    out.push_back(Source{centerX, centerY, amplitude});
    for (int k = 1; k <= rings; ++k) {
        const double r = radius * static_cast<double>(k) / static_cast<double>(rings);
        const int nTheta = std::max(6, 6 * k);
        for (int m = 0; m < nTheta; ++m) {
            const double a = 2.0 * kPi * static_cast<double>(m) / static_cast<double>(nTheta);
            Source s;
            s.x = centerX + r * std::cos(a);
            s.y = centerY + r * std::sin(a);
            s.amplitude = amplitude;
            out.push_back(s);
        }
    }
    return out;
}

}  // namespace ringphantom
