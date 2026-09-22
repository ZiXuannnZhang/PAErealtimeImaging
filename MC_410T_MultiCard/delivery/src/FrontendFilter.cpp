#include "FrontendFilter.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <utility>

namespace frontend_filter {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

// Direct II transposed biquad recursion, a0 = 1, zero initial state:
//   y[n] = b0*x[n] + z1
//   z1   = b1*x[n] - a1*y[n] + z2
//   z2   = b2*x[n] - a2*y[n]
void filterSection(const Section& s, float* x, std::size_t n) noexcept {
    double z1 = 0.0, z2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double in = static_cast<double>(x[i]);
        const double out = s.b0 * in + z1;
        z1 = s.b1 * in - s.a1 * out + z2;
        z2 = s.b2 * in - s.a2 * out;
        x[i] = static_cast<float>(out);
    }
}

// 端部延拓的唯一实现。oddReflectExtend() 与 filtfilt() 都经由本函数，因此测试
// 对 oddReflectExtend 的逐点断言即是对生产延拓规则的断言。
template <class T>
void oddReflectExtendInto(const float* x, std::size_t n, std::size_t L, T* out) noexcept {
    for (std::size_t i = 0; i < L; ++i) {
        // 左端第 k 点（k = 1 … L，自 x[0] 向外）取 2*x[0] - x[k]；缓冲区最左为 k = L。
        const std::size_t k = L - i;
        out[i] = static_cast<T>(2.0 * static_cast<double>(x[0]) -
                                static_cast<double>(x[k]));
    }
    for (std::size_t i = 0; i < n; ++i) out[L + i] = static_cast<T>(x[i]);
    for (std::size_t j = 1; j <= L; ++j) {
        // 右端第 j 点（j = 1 … L，自 x[n-1] 向外）取 2*x[n-1] - x[n-1-j]。
        out[L + n + j - 1] = static_cast<T>(
            2.0 * static_cast<double>(x[n - 1]) -
            static_cast<double>(x[n - 1 - j]));
    }
}

void requireOrder(int order) {
    if (order < kMinOrder || order > kMaxOrder)
        throw std::invalid_argument("frontend filter order out of range 1..8");
}

void requireCutoff(double cutoffHz) {
    const double nyquist = kSampleRateHz / 2.0;
    if (!(cutoffHz > 0.0) || !(cutoffHz < nyquist))
        throw std::invalid_argument("frontend filter cutoff out of range (0, Nyquist)");
}

// 把增益归一化到参考频点（低通 DC z=1，高通 Nyquist z=-1）。系数产出时 a0 已为 1。
void normalizeSection(Section& s, bool highpass) {
    // 增益：H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
    //   z =  1 -> b0 + b1 + b2 / 1 + a1 + a2
    //   z = -1 -> b0 - b1 + b2 / 1 - a1 + a2
    const double num = highpass ? (s.b0 - s.b1 + s.b2) : (s.b0 + s.b1 + s.b2);
    const double den = highpass ? (1.0 - s.a1 + s.a2) : (1.0 + s.a1 + s.a2);
    if (num == 0.0 || den == 0.0)
        throw std::runtime_error("frontend filter section normalization is singular");
    const double gain = num / den;
    s.b0 /= gain;
    s.b1 /= gain;
    s.b2 /= gain;
}

// 模拟原型一对共轭极点 -> 一个二阶节（双线性变换后）。
//   分母 s^2 + alpha*s + beta，其中 alpha = -2*Re(p) > 0，beta = |p|^2 = Ω^2。
//   低通分子 beta（零点在无穷远）；高通分子 s^2（零点在 s = 0）。
Section designPairSection(double alpha, double beta, bool highpass) {
    const double K = 2.0 * kSampleRateHz;  // 双线性变换 s = K (z-1)/(z+1)
    const double K2 = K * K;

    // s = K(z-1)/(z+1) 代入并同乘 (z+1)^2：
    //   分母 -> c0 z^2 + c1 z + c2
    const double c0 = K2 + alpha * K + beta;
    const double c1 = -2.0 * K2 + 2.0 * beta;
    const double c2 = K2 - alpha * K + beta;
    if (c0 == 0.0)
        throw std::runtime_error("frontend filter bilinear transform is singular");

    Section s;
    if (highpass) {
        // 分子 s^2 -> K^2 (z-1)^2 = K^2 z^2 - 2 K^2 z + K^2
        s.b0 = K2 / c0;
        s.b1 = -2.0 * K2 / c0;
        s.b2 = K2 / c0;
    } else {
        // 分子 beta -> beta (z+1)^2 = beta z^2 + 2 beta z + beta
        s.b0 = beta / c0;
        s.b1 = 2.0 * beta / c0;
        s.b2 = beta / c0;
    }
    s.a1 = c1 / c0;
    s.a2 = c2 / c0;
    normalizeSection(s, highpass);
    return s;
}

// 奇数阶时的实极点 p = -Ω -> 一阶节（b2 = a2 = 0）。
//   低通分子 Ω；高通分子 s。
Section designRealPoleSection(double omega, bool highpass) {
    const double K = 2.0 * kSampleRateHz;
    const double c0 = K + omega;
    const double c1 = omega - K;
    if (c0 == 0.0)
        throw std::runtime_error("frontend filter bilinear transform is singular");

    Section s;
    if (highpass) {
        // 分子 s -> K(z-1) = K z - K
        s.b0 = K / c0;
        s.b1 = -K / c0;
        s.b2 = 0.0;
    } else {
        // 分子 Ω -> Ω(z+1) = Ω z + Ω
        s.b0 = omega / c0;
        s.b1 = omega / c0;
        s.b2 = 0.0;
    }
    s.a1 = c1 / c0;
    s.a2 = 0.0;
    normalizeSection(s, highpass);
    return s;
}

// 高低通共用的设计流程。highpass 决定零点位置（无穷远 vs s = 0）与增益归一化频点。
Coefficients designPath(bool highpass, int order, double cutoffHz) {
    requireOrder(order);
    requireCutoff(cutoffHz);

    // 步骤 1：预畸变模拟截止 Ω = 2 · fs · tan(π · fc / fs)。
    const double fs = kSampleRateHz;
    const double omega = 2.0 * fs * std::tan(kPi * cutoffHz / fs);
    if (!(omega > 0.0) || !std::isfinite(omega))
        throw std::runtime_error("frontend filter prewarp produced a non-finite cutoff");

    Coefficients out;
    out.order = order;

    // 步骤 2：N 阶模拟 Butterworth 原型极点 p_k = Ω · exp(j · π · (2k + N − 1)/(2N))。
    //  θ_{N+1-k} = 2π − θ_k，故 k 与 N+1-k 构成共轭对；N 为奇数时 k = (N+1)/2
    //  落在 θ = π，即实极点 p = −Ω。
    std::vector<std::complex<double>> poles(static_cast<std::size_t>(order));
    for (int k = 1; k <= order; ++k) {
        const double theta = kPi * (2.0 * k + order - 1.0) / (2.0 * order);
        poles[static_cast<std::size_t>(k - 1)] =
            omega * std::polar(1.0, theta);
    }

    // 步骤 5/6：共轭对 -> 二阶节；奇数阶再补一个一阶节。按节顺序级联。
    out.sections.reserve(static_cast<std::size_t>((order + 1) / 2));
    for (int k = 1; k <= order / 2; ++k) {
        const std::complex<double> p = poles[static_cast<std::size_t>(k - 1)];
        const double alpha = -2.0 * p.real();   // = 2Ω|cosθ| > 0
        const double beta = std::norm(p);       // = Ω^2
        out.sections.push_back(designPairSection(alpha, beta, highpass));
    }
    if (order % 2 == 1) {
        out.sections.push_back(designRealPoleSection(omega, highpass));
        // 一阶节固定放在级联末尾，b2 = a2 = 0（任务 3.3 步骤 5）。
    }
    return out;
}

}  // namespace

const char* validationMessage(Validation result) noexcept {
    switch (result) {
    case Validation::Ok:            return "ok";
    case Validation::HpCutoffRange: return "高通截止频率必须大于 0 且小于 125 MHz";
    case Validation::HpOrderRange:  return "高通阶数必须在 1 ~ 8 之间";
    case Validation::LpCutoffRange: return "低通截止频率必须大于 0 且小于 125 MHz";
    case Validation::LpOrderRange:  return "低通阶数必须在 1 ~ 8 之间";
    case Validation::BandOrder:     return "两路都启用时，低通截止频率必须严格大于高通截止频率";
    }
    return "未知的前端滤波参数错误";
}

Validation validate(const Config& config) noexcept {
    // 截止频率与阶数对两路都按 3.4 独立检查（无论该路是否启用），避免停用路
    // 留下越界值后重新启用时才暴露。
    if (!(config.hpCutoffMhz > 0.0) || !(config.hpCutoffMhz < kNyquistMhz))
        return Validation::HpCutoffRange;
    if (config.hpOrder < kMinOrder || config.hpOrder > kMaxOrder)
        return Validation::HpOrderRange;
    if (!(config.lpCutoffMhz > 0.0) || !(config.lpCutoffMhz < kNyquistMhz))
        return Validation::LpCutoffRange;
    if (config.lpOrder < kMinOrder || config.lpOrder > kMaxOrder)
        return Validation::LpOrderRange;
    if (config.hpEnable && config.lpEnable &&
        !(config.lpCutoffMhz > config.hpCutoffMhz))
        return Validation::BandOrder;
    return Validation::Ok;
}

std::size_t extensionLength(std::size_t n, int order) noexcept {
    // n < 2：该路对本序列直接原样返回，不需要延拓。
    if (n < 2) return 0;
    const std::size_t full = static_cast<std::size_t>(
        std::max(order, kMinOrder) * 3);            // L = 3 × 阶数
    return std::min(full, n - 1);                   // n ≤ L 时取 L = n − 1
}

std::vector<float> oddReflectExtend(const float* x, std::size_t n, std::size_t L) {
    if (n == 0) return {};
    if (L > n - 1) throw std::invalid_argument("extension length must be <= n - 1");
    std::vector<float> out(n + 2 * L);
    oddReflectExtendInto<float>(x, n, L, out.data());
    return out;
}

Coefficients designLowpass(int order, double cutoffHz) {
    return designPath(false, order, cutoffHz);
}

Coefficients designHighpass(int order, double cutoffHz) {
    return designPath(true, order, cutoffHz);
}

void filterForward(const Coefficients& coeff, float* x, std::size_t n) {
    if (n == 0 || !x) return;
    for (const Section& section : coeff.sections) filterSection(section, x, n);
}

void filtfilt(const Coefficients& coeff, float* x, std::size_t n) {
    if (!x || n == 0) return;
    // 任务 3.5：n < 2 时该路对本序列直接原样返回（不做系数运算，也不改写）。
    if (n < 2 || coeff.empty()) return;

    // 步骤 1：端部延拓，两端各 L = 3 × 阶数 点（n ≤ L 时取 L = n − 1）。
    const std::size_t L = extensionLength(n, coeff.order);
    std::vector<float> work(n + 2 * L);
    oddReflectExtendInto<float>(x, n, L, work.data());

    // 步骤 2：正向滤波一次，滤波器初始状态置零。
    filterForward(coeff, work.data(), work.size());
    // 步骤 3：时间反转，再经同一 SOS 级联滤波一次，滤波器初始状态置零。
    std::reverse(work.begin(), work.end());
    filterForward(coeff, work.data(), work.size());
    // 步骤 4：时间反转，截去两端各 L 点，长度回到 n，写回目标缓冲。
    std::reverse(work.begin(), work.end());
    for (std::size_t i = 0; i < n; ++i) x[i] = work[L + i];
}

Bank::Bank(const Config& config) : config_(config) {
    if (validate(config) != Validation::Ok)
        throw std::invalid_argument("frontend filter config rejected by validation");
    // 未启用的路不做系数设计，也不做运算（任务 3.3）。
    if (config.hpEnable) {
        hp_ = designHighpass(config.hpOrder, config.hpCutoffMhz * 1e6);
        if (hp_.empty())
            throw std::runtime_error("frontend highpass design produced no sections");
    }
    if (config.lpEnable) {
        lp_ = designLowpass(config.lpOrder, config.lpCutoffMhz * 1e6);
        if (lp_.empty())
            throw std::runtime_error("frontend lowpass design produced no sections");
    }
}

bool Bank::apply(float* x, std::size_t n) const {
    if (!x || n == 0) return false;
    // n < 2：两路都对本序列直接原样返回，没有一路真正执行滤波。
    if (n < 2) return false;
    bool didFilter = false;
    // 两路都启用时的执行顺序：先高通、后低通；每一路各是一次完整的前向-后向滤波，
    // 即高通的输出作为低通的输入。
    if (!hp_.empty()) {
        filtfilt(hp_, x, n);
        didFilter = true;
    }
    if (!lp_.empty()) {
        filtfilt(lp_, x, n);
        didFilter = true;
    }
    return didFilter;
}

}  // namespace frontend_filter
