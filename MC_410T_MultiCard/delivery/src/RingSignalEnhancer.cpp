#include "RingSignalEnhancer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::size_t nextPowerOfTwo(std::size_t value) {
    std::size_t result = 1;
    while (result < value) result <<= 1;
    return result;
}

// A3 实 FFT 快速路径：反射延拓后信号为实且对称，DFT 为 Hermitian——
// 用 pack-trick 把 N 点实 FFT 化为 N/2 点复 FFT（复用同一 radix-2 引擎），
// 正向与逆向同享 2×；增益表只存半谱 k=0..N/2（DC 与 Nyquist 单独处理）。
// 数学定义不变：仍是 H(f)=1+(Hmax-1)*exp(-(f/fc)^order) 乘谱后逆变换。
struct FFTPlan {
    std::size_t size = 0;      // fftSize = N（2 的幂，>=2）
    std::size_t halfSize = 0;  // M = N/2（transform 的实际点数）
    std::vector<std::size_t> bitReversal;          // M 项
    std::vector<std::complex<double>> twiddles;    // M/2 项（M 点变换旋转因子）
    std::vector<double> freqGain;                  // 半谱 M+1 项：G[0..M]
    std::vector<std::complex<double>> halfTwiddles;  // W^k = e^{-2πik/N}，k=0..M-1

    void build(std::size_t fftSize, double fs, const RingEnhanceConfig &config) {
        size = fftSize;
        halfSize = fftSize / 2;
        const std::size_t m = halfSize;
        bitReversal.resize(m);
        unsigned bits = 0;
        for (std::size_t value = m; value > 1; value >>= 1) ++bits;
        for (std::size_t i = 0; i < m; ++i) {
            std::size_t x = i;
            std::size_t y = 0;
            for (unsigned b = 0; b < bits; ++b) {
                y = (y << 1) | (x & 1u);
                x >>= 1;
            }
            bitReversal[i] = y;
        }

        twiddles.resize(m / 2);
        for (std::size_t k = 0; k < twiddles.size(); ++k) {
            const double angle =
                -2.0 * kPi * static_cast<double>(k) / static_cast<double>(m);
            twiddles[k] = {std::cos(angle), std::sin(angle)};
        }

        halfTwiddles.resize(m);
        for (std::size_t k = 0; k < m; ++k) {
            const double angle =
                -2.0 * kPi * static_cast<double>(k) / static_cast<double>(fftSize);
            halfTwiddles[k] = {std::cos(angle), std::sin(angle)};
        }

        freqGain.resize(m + 1);
        for (std::size_t k = 0; k <= m; ++k) {
            double frequency =
                static_cast<double>(k) * fs / static_cast<double>(fftSize);
            frequency = std::min(frequency, fs - frequency);
            freqGain[k] = ring_enhance::frequencyGain(frequency, config);
        }
    }

    // M 点 radix-2 复 FFT；inverse 含 1/M 归一化（与原实现同式）。
    void transform(std::vector<std::complex<double>> &values, bool inverse) const {
        const std::size_t n = values.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = bitReversal[i];
            if (i < j) std::swap(values[i], values[j]);
        }

        for (std::size_t len = 2; len <= n; len <<= 1) {
            const std::size_t half = len >> 1;
            const std::size_t step = n / len;
            for (std::size_t base = 0; base < n; base += len) {
                for (std::size_t j = 0; j < half; ++j) {
                    std::complex<double> w = twiddles[j * step];
                    if (inverse) w = std::conj(w);
                    const std::complex<double> even = values[base + j];
                    const std::complex<double> odd =
                        values[base + j + half] * w;
                    values[base + j] = even + odd;
                    values[base + j + half] = even - odd;
                }
            }
        }

        if (inverse) {
            const double scale = 1.0 / static_cast<double>(n);
            for (auto &value : values) value *= scale;
        }
    }
};

}  // namespace

namespace ring_enhance {

bool validate(const RingEnhanceConfig &config, const char **error) {
    auto fail = [error](const char *message) {
        if (error) *error = message;
        return false;
    };
    if (!std::isfinite(config.freqCompFcMhz) || config.freqCompFcMhz <= 0.0)
        return fail("freqCompFcMhz must be positive and finite");
    if (!std::isfinite(config.freqCompHmax) || config.freqCompHmax < 1.0)
        return fail("freqCompHmax must be finite and >= 1");
    if (!std::isfinite(config.freqCompOrder) || config.freqCompOrder <= 0.0)
        return fail("freqCompOrder must be positive and finite");
    if (error) *error = nullptr;
    return true;
}

double frequencyGain(double frequencyHz, const RingEnhanceConfig &config) {
    const double normalized =
        std::max(0.0, frequencyHz) / (config.freqCompFcMhz * 1e6);
    return 1.0 + (config.freqCompHmax - 1.0) *
                     std::exp(-std::pow(normalized, config.freqCompOrder));
}

}  // namespace ring_enhance

struct RingSignalEnhancer::Impl {
    RingEnhanceConfig config;
    double fs = 1.0;
    std::unordered_map<int, FFTPlan> plans;
    // 计划指针缓存：ImagingSvc 逐 A-line 以同一 sampleCount 连续调用，
    // 免去热路径上的 unordered_map 查找；setConfig 变更时失效。
    const FFTPlan *lastPlan = nullptr;
    int lastPlanCount = -1;
    // 逐 A-line 复用缓冲（A2）：消除每次 apply 的堆分配；按需增长，
    // 容量跨调用保留。缓冲内容在每次使用前整体重写，不影响数值结果。
    std::vector<double> work;
    std::vector<double> derivative;
    std::vector<std::complex<double>> values;   // A3：N/2 点 packed 频域/时域复缓冲
    std::vector<std::complex<double>> halfY;    // A3：半谱 Y[0..M] 暂存

    bool enabled() const {
        return config.enableBipolarCompensation ||
               config.enableFreqCompensation;
    }

    bool apply(float *samples, int sampleCount) {
        if (!samples || sampleCount <= 0) return false;
        if (!enabled()) return true;

        work.assign(samples, samples + sampleCount);
        if (config.enableFreqCompensation &&
            !applyFrequencyGain(work, sampleCount)) {
            return false;
        }
        if (config.enableBipolarCompensation) {
            if (sampleCount < 2) return false;
            applyBipolarCompensation(work);
        }

        for (int i = 0; i < sampleCount; ++i) {
            samples[i] = static_cast<float>(work[static_cast<std::size_t>(i)]);
        }
        return true;
    }

private:
    bool applyFrequencyGain(std::vector<double> &samples, int sampleCount) {
        const std::size_t fftSize =
            nextPowerOfTwo(static_cast<std::size_t>(std::max(1, sampleCount)));
        if (fftSize < 2) {
            // fftSize == 1：无频谱结构，等价于原全谱路径（增益 = H(0)，乘后取实）
            if (lastPlan == nullptr || lastPlanCount != sampleCount) {
                FFTPlan &plan = plans[sampleCount];
                if (plan.size != fftSize) {
                    plan.build(fftSize, fs, config);
                }
                lastPlan = &plan;
                lastPlanCount = sampleCount;
            }
            samples[0] = static_cast<float>(
                static_cast<double>(samples[0]) * ring_enhance::frequencyGain(0.0, config));
            return true;
        }
        if (lastPlan == nullptr || lastPlanCount != sampleCount) {
            FFTPlan &plan = plans[sampleCount];
            if (plan.size != fftSize || plan.freqGain.size() != fftSize / 2 + 1) {
                plan.build(fftSize, fs, config);
            }
            lastPlan = &plan;
            lastPlanCount = sampleCount;
        }
        const FFTPlan &plan = *lastPlan;
        const std::size_t m = plan.halfSize;
        const int extra = static_cast<int>(fftSize) - sampleCount;
        const long long left = extra / 2;

        // pack：z[n] = x[2n] + i·x[2n+1]，x 为与原实现相同的反射延拓序列
        values.resize(m);
        for (std::size_t n = 0; n < m; ++n) {
            const long long s0 = 2LL * static_cast<long long>(n) - left;
            values[n] = {reflectSample(samples, s0, sampleCount),
                         reflectSample(samples, s0 + 1, sampleCount)};
        }
        plan.transform(values, false);

        // 正向解包 + 半谱增益：X[k] = Ev + W^k·Od，Y = G·X；X[N/2]（Nyquist）实
        halfY.resize(m + 1);
        const std::complex<double> halfI(0.0, -0.5);   // 1/(2i)
        for (std::size_t k = 0; k < m; ++k) {
            const std::size_t kmir = (m - k) % m;
            const std::complex<double> zm = std::conj(values[kmir]);
            const std::complex<double> ev = 0.5 * (values[k] + zm);
            const std::complex<double> od = halfI * (values[k] - zm);
            halfY[k] = plan.freqGain[k] * (ev + plan.halfTwiddles[k] * od);
        }
        {
            // Nyquist（k=M）：X[M] = Re(Z0) − Im(Z0)（实数）
            const std::complex<double> ev0 =
                0.5 * (values[0] + std::conj(values[0]));
            const std::complex<double> od0 = halfI * (values[0] - std::conj(values[0]));
            halfY[m] = plan.freqGain[m] * (ev0 - od0);
        }

        // 逆向打包：z 的 M 点谱 Zinv[k] = EvY[k] + i·OdY[k]，其中 EvY/OdY 由
        // 半谱 Hermitian 关系得到（Y[M+k] = conj(Y[M-k])）：
        //   k=0：EvY=(Y[0]+Y[M])/2，OdY=(Y[0]-Y[M])/2（Nyquist 实值单独处理）
        //   k≥1：EvY=(Y[k]+conj(Y[M-k]))/2，OdY=W^{-k}·(Y[k]-conj(Y[M-k]))/2
        // IDFT_M(Zinv) = z = y 偶序列 + i·y 奇序列（1/M 归一化在 transform 内）
        const std::complex<double> iUnit(0.0, 1.0);
        for (std::size_t k = 0; k < m; ++k) {
            std::complex<double> evY, odY;
            if (k == 0) {
                evY = 0.5 * (halfY[0] + halfY[m]);
                odY = 0.5 * (halfY[0] - halfY[m]);
            } else {
                const std::complex<double> ym = std::conj(halfY[m - k]);
                evY = 0.5 * (halfY[k] + ym);
                odY = 0.5 * std::conj(plan.halfTwiddles[k]) * (halfY[k] - ym);
            }
            values[k] = evY + iUnit * odY;
        }
        plan.transform(values, true);

        // 时域解包：y[2n] = Re(P[n])，y[2n+1] = Im(P[n])
        // 注意保留 double 中间精度（float 化只在 apply() 末尾发生一次，
        // 与原实现的双极补偿输入口径一致）
        for (int i = 0; i < sampleCount; ++i) {
            const std::size_t j = static_cast<std::size_t>(left) +
                                  static_cast<std::size_t>(i);
            const std::complex<double> &p = values[j >> 1];
            samples[i] = ((j & 1ULL) != 0) ? p.imag() : p.real();
        }
        return true;
    }

    static double reflectSample(const std::vector<double> &samples,
                                long long index, int sampleCount) {
        if (sampleCount <= 0) return 0.0;
        if (sampleCount == 1) return samples.front();
        const long long period = 2LL * (sampleCount - 1);
        long long wrapped = index % period;
        if (wrapped < 0) wrapped += period;
        if (wrapped >= sampleCount) wrapped = period - wrapped;
        return samples[static_cast<std::size_t>(wrapped)];
    }

    void applyBipolarCompensation(std::vector<double> &samples) {
        const int n = static_cast<int>(samples.size());
        derivative.resize(samples.size());
        derivative.front() = (samples[1] - samples[0]) * fs;
        derivative.back() =
            (samples.back() - samples[samples.size() - 2]) * fs;
        for (int i = 1; i < n - 1; ++i) {
            derivative[static_cast<std::size_t>(i)] =
                (samples[static_cast<std::size_t>(i + 1)] -
                 samples[static_cast<std::size_t>(i - 1)]) *
                (fs * 0.5);
        }
        for (int i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / fs;
            samples[static_cast<std::size_t>(i)] -=
                t * derivative[static_cast<std::size_t>(i)];
        }
    }
};

RingSignalEnhancer::RingSignalEnhancer()
    : impl_(std::make_unique<Impl>()) {}

RingSignalEnhancer::~RingSignalEnhancer() = default;

void RingSignalEnhancer::setConfig(const RingEnhanceConfig &config,
                                   double sampleRateHz) {
    impl_->fs = sampleRateHz;
    impl_->config = config;
    impl_->plans.clear();
    impl_->lastPlan = nullptr;
    impl_->lastPlanCount = -1;
}

const RingEnhanceConfig &RingSignalEnhancer::config() const {
    return impl_->config;
}

bool RingSignalEnhancer::enabled() const { return impl_->enabled(); }

bool RingSignalEnhancer::apply(float *samples, int sampleCount) {
    return impl_->apply(samples, sampleCount);
}
