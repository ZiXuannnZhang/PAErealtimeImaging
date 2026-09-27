// ring_enhancer_parity_test — 增强算子新旧路径对拍（优化任务 A2/A3 验收）。
//
// 参考实现 = 预算测试任务 1 时的 RingSignalEnhancer 算法逐字转录
// （整样本对称反射延拓 + 全谱复 radix-2 FFT/IFFT + 全谱实增益 + 中心差分双极补偿）。
// 本测试将其与生产 RingSignalEnhancer 的当前实现逐位比较：
//   - A2（调用开销消除）要求 0 差异：--max-ulp 0；
//   - A3（实 FFT 快速路径）预注册口径：float 输出目标逐位一致，允许个别 ±1 ULP，
//     每个差异样本打印（下标/参考值/实际值/ULP 距离）供证据归因。
// 输入：固定种子随机实信号（多种长度/形态）+ 实录 14.dat A-line 列（wlOffset=301，
// 3830 = 4000−171+1 保留段语义，fs=250e6）。
// 用法：ring_enhancer_parity_test [--data <14.dat>] [--max-ulp N] [--max-diff-samples K]
#include "RingSignalEnhancer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kWlOffset = 301;     // 14.dat 口径
constexpr int kSampDepth = 4000;
constexpr int kLenDefault = 3830;  // 4000 - 171 + 1

std::string argStr(const std::vector<std::string> &args, const std::string &key,
                   const std::string &def = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return args[i + 1];
    return def;
}
int argInt(const std::vector<std::string> &args, const std::string &key, int def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return std::stoi(args[i + 1]);
    return def;
}

// ===================== 参考实现（任务 1 时算法的逐字转录，勿改动算术次序） =====================
namespace reference {

constexpr double kPi = 3.14159265358979323846;

std::size_t nextPowerOfTwo(std::size_t value) {
    std::size_t result = 1;
    while (result < value) result <<= 1;
    return result;
}

struct FFTPlan {
    std::size_t size = 0;
    std::vector<std::size_t> bitReversal;
    std::vector<std::complex<double>> twiddles;
    std::vector<double> freqGain;

    void build(std::size_t fftSize, double fs, const RingEnhanceConfig &config) {
        size = fftSize;
        bitReversal.resize(fftSize);
        unsigned bits = 0;
        for (std::size_t value = fftSize; value > 1; value >>= 1) ++bits;
        for (std::size_t i = 0; i < fftSize; ++i) {
            std::size_t x = i;
            std::size_t y = 0;
            for (unsigned b = 0; b < bits; ++b) {
                y = (y << 1) | (x & 1u);
                x >>= 1;
            }
            bitReversal[i] = y;
        }
        twiddles.resize(fftSize / 2);
        for (std::size_t k = 0; k < twiddles.size(); ++k) {
            const double angle =
                -2.0 * kPi * static_cast<double>(k) / static_cast<double>(fftSize);
            twiddles[k] = {std::cos(angle), std::sin(angle)};
        }
        freqGain.resize(fftSize);
        for (std::size_t k = 0; k < fftSize; ++k) {
            double frequency =
                static_cast<double>(k) * fs / static_cast<double>(fftSize);
            frequency = std::min(frequency, fs - frequency);
            freqGain[k] = ring_enhance::frequencyGain(frequency, config);
        }
    }

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

struct RefImpl {
    RingEnhanceConfig config;
    double fs = 1.0;
    std::unordered_map<int, FFTPlan> plans;

    bool enabled() const {
        return config.enableBipolarCompensation || config.enableFreqCompensation;
    }

    bool apply(float *samples, int sampleCount) {
        if (!samples || sampleCount <= 0) return false;
        if (!enabled()) return true;
        std::vector<double> work(samples, samples + sampleCount);
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
        auto &plan = plans[sampleCount];
        if (plan.size != fftSize || plan.freqGain.size() != fftSize) {
            plan.build(fftSize, fs, config);
        }
        std::vector<std::complex<double>> values(fftSize, {0.0, 0.0});
        const int extra = static_cast<int>(fftSize) - sampleCount;
        const int left = extra / 2;
        for (std::size_t out = 0; out < fftSize; ++out) {
            const long long source =
                static_cast<long long>(out) - static_cast<long long>(left);
            values[out] = {reflectSample(samples, source, sampleCount), 0.0};
        }
        plan.transform(values, false);
        for (std::size_t i = 0; i < fftSize; ++i) values[i] *= plan.freqGain[i];
        plan.transform(values, true);
        for (int i = 0; i < sampleCount; ++i) {
            samples[static_cast<std::size_t>(i)] =
                values[static_cast<std::size_t>(left + i)].real();
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
        std::vector<double> derivative(samples.size(), 0.0);
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

}  // namespace reference
// ===================== 参考实现结束 =====================

bool readColumn(const std::string &path, int sampDepth, long long col0,
                std::vector<double> &col) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(col0 * static_cast<long long>(sampDepth) * 8);
    col.resize(static_cast<size_t>(sampDepth));
    f.read(reinterpret_cast<char *>(col.data()),
           static_cast<std::streamsize>(sampDepth) * 8);
    return f.good() || f.gcount() == static_cast<std::streamsize>(sampDepth) * 8;
}

int failures = 0;
void require(bool ok, const char *message) {
    if (!ok) {
        ++failures;
        std::printf("FAIL  %s\n", message);
    }
}

// 逐位比较 + ULP 分类（float 位模式距离）
int floatUlpDistance(float a, float b) {
    int ia, ib;
    std::memcpy(&ia, &a, 4);
    std::memcpy(&ib, &b, 4);
    if (ia < 0) ia = 0x80000000 - ia;   // 符号位映射到有序整数空间
    if (ib < 0) ib = 0x80000000 - ib;
    return std::abs(ia - ib);
}

struct CaseResult {
    long long diffs = 0;
    int maxUlp = 0;
    bool allUlpWithinPolicy = true;
};

CaseResult compareCase(const char *label, const RingEnhanceConfig &cfg, double fs,
                       const std::vector<float> &input, int maxUlp,
                       long long &printed) {
    std::vector<float> actual = input;
    std::vector<float> expected = input;
    RingSignalEnhancer enhancer;
    enhancer.setConfig(cfg, fs);
    reference::RefImpl ref;
    ref.config = cfg;
    ref.fs = fs;
    const bool okA = enhancer.apply(actual.data(), static_cast<int>(actual.size()));
    const bool okB = ref.apply(expected.data(), static_cast<int>(expected.size()));
    CaseResult result;
    if (!okA || !okB) {
        if (!okA && !okB) {
            // 两侧一致的参数拒绝（如 sampleCount<2 且双极补偿开启）——对齐行为，非差异
            std::printf("CASE  %s len=%zu rejected-both (aligned)\n", label,
                        input.size());
            return result;
        }
        std::printf("CASE  %s len=%zu apply-failed actual=%d ref=%d\n", label,
                    input.size(), okA ? 1 : 0, okB ? 1 : 0);
        result.allUlpWithinPolicy = false;
        ++failures;
        return result;
    }
    if (cfg.enableBipolarCompensation || cfg.enableFreqCompensation) {
        for (size_t i = 0; i < input.size(); ++i) {
            if (std::memcmp(&actual[i], &expected[i], 4) != 0) {
                const int ulp = floatUlpDistance(actual[i], expected[i]);
                ++result.diffs;
                result.maxUlp = std::max(result.maxUlp, ulp);
                if (ulp > maxUlp) result.allUlpWithinPolicy = false;
                if (printed < 200) {
                    std::printf("DIFF  %s len=%zu idx=%zu ref=%.9g act=%.9g ulp=%d\n",
                                label, input.size(), i,
                                static_cast<double>(expected[i]),
                                static_cast<double>(actual[i]), ulp);
                    ++printed;
                }
            }
        }
    } else {
        // off 恒等：必须逐位一致（不入 ULP 政策）
        for (size_t i = 0; i < input.size(); ++i) {
            if (std::memcmp(&actual[i], &expected[i], 4) != 0) {
                ++result.diffs;
                result.allUlpWithinPolicy = false;
                if (printed < 200) {
                    std::printf("DIFF  %s len=%zu idx=%zu off-path-not-identity\n",
                                label, input.size(), i);
                    ++printed;
                }
            }
        }
    }
    std::printf("CASE  %s len=%zu fs=%g bipolar=%d freq=%d hmax=%g diffs=%lld max_ulp=%d\n",
                label, input.size(), fs,
                cfg.enableBipolarCompensation ? 1 : 0,
                cfg.enableFreqCompensation ? 1 : 0, cfg.freqCompHmax,
                result.diffs, result.maxUlp);
    return result;
}

}  // namespace

int main(int argc, char **argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string dataPath = argStr(args, "--data");
    const int maxUlp = argInt(args, "--max-ulp", 1);
    const long long maxDiffSamples = argInt(args, "--max-diff-samples", -1);

    long long printed = 0;
    long long totalDiffs = 0;
    int totalMaxUlp = 0;
    bool policyOk = true;

    // 随机实信号矩阵（固定种子可复现）
    struct SizeSpec { int length; const char *kind; };
    const SizeSpec sizes[] = {
        {1, "edge"}, {2, "edge"}, {3, "edge"}, {511, "odd"},
        {512, "pow2"}, {513, "odd"}, {1000, "round"}, {3830, "default"},
        {4000, "full-col"}, {4096, "pow2"}, {8000, "round"}, {12330, "large"}};
    std::mt19937 rng(20260927u);
    std::uniform_real_distribution<double> uniform(-1.0, 1.0);
    for (const auto &spec : sizes) {
        std::vector<std::vector<float>> lines;
        for (int variant = 0; variant < 4; ++variant) {
            std::vector<float> line(static_cast<size_t>(spec.length));
            switch (variant) {
                case 0:   // 常数
                    std::fill(line.begin(), line.end(), 3.25f);
                    break;
                case 1:   // 斜坡
                    for (int i = 0; i < spec.length; ++i)
                        line[static_cast<size_t>(i)] = static_cast<float>(
                            static_cast<double>(i) / 250e6);
                    break;
                case 2:   // 单脉冲
                    for (int i = 0; i < spec.length; ++i) {
                        const double u = (static_cast<double>(i) / 250e6 -
                                          0.25 * spec.length / 250e6) / 0.2e-6;
                        line[static_cast<size_t>(i)] = static_cast<float>(
                            -u * std::exp(-0.5 * u * u));
                    }
                    break;
                default:  // 噪声
                    for (int i = 0; i < spec.length; ++i)
                        line[static_cast<size_t>(i)] = static_cast<float>(uniform(rng) * 500.0);
                    break;
            }
            lines.push_back(std::move(line));
        }
        for (const auto &line : lines) {
            for (double hmax : {2.0, 20.0}) {
                RingEnhanceConfig cfgFreq;
                cfgFreq.enableFreqCompensation = true;
                cfgFreq.freqCompHmax = hmax;
                const CaseResult r = compareCase("random/freq", cfgFreq, 250e6,
                                                 line, maxUlp, printed);
                totalDiffs += r.diffs; totalMaxUlp = std::max(totalMaxUlp, r.maxUlp);
                policyOk = policyOk && r.allUlpWithinPolicy;
            }
            RingEnhanceConfig cfgBoth;
            cfgBoth.enableBipolarCompensation = true;
            cfgBoth.enableFreqCompensation = true;
            const CaseResult r = compareCase("random/both", cfgBoth, 250e6,
                                             line, maxUlp, printed);
            totalDiffs += r.diffs; totalMaxUlp = std::max(totalMaxUlp, r.maxUlp);
            policyOk = policyOk && r.allUlpWithinPolicy;
            RingEnhanceConfig cfgBip;
            cfgBip.enableBipolarCompensation = true;
            const CaseResult rb = compareCase("random/bipolar", cfgBip, 250e6,
                                              line, maxUlp, printed);
            totalDiffs += rb.diffs; totalMaxUlp = std::max(totalMaxUlp, rb.maxUlp);
            policyOk = policyOk && rb.allUlpWithinPolicy;
        }
    }

    // 实录 14.dat A-line（wl1/wl2 各半）
    if (!dataPath.empty()) {
        bool allOk = true;
        for (int i = 0; i < 16 && allOk; ++i) {
            const int wlIdx = (i / 2) * 500;
            const int wl = (i % 2 == 0) ? 0 : 1;
            const long long col0 = kWlOffset - 1 + 2LL * wlIdx + wl;
            std::vector<double> col;
            if (!readColumn(dataPath, kSampDepth, col0, col)) { allOk = false; break; }
            std::vector<float> line(static_cast<size_t>(kLenDefault));
            for (int r = 0; r < kLenDefault; ++r)
                line[static_cast<size_t>(r)] =
                    static_cast<float>(col[static_cast<size_t>(r + (kSampDepth - kLenDefault))]);
            for (double hmax : {2.0, 20.0}) {
                RingEnhanceConfig cfgFreq;
                cfgFreq.enableFreqCompensation = true;
                cfgFreq.freqCompHmax = hmax;
                const CaseResult r = compareCase("real14/freq", cfgFreq, 250e6,
                                                 line, maxUlp, printed);
                totalDiffs += r.diffs; totalMaxUlp = std::max(totalMaxUlp, r.maxUlp);
                policyOk = policyOk && r.allUlpWithinPolicy;
            }
            RingEnhanceConfig cfgBoth;
            cfgBoth.enableBipolarCompensation = true;
            cfgBoth.enableFreqCompensation = true;
            const CaseResult r = compareCase("real14/both", cfgBoth, 250e6,
                                             line, maxUlp, printed);
            totalDiffs += r.diffs; totalMaxUlp = std::max(totalMaxUlp, r.maxUlp);
            policyOk = policyOk && r.allUlpWithinPolicy;
        }
        if (!allOk) {
            std::printf("FAIL  real14 data unreadable (%s)\n", dataPath.c_str());
            ++failures;
        }
    }

    std::printf("SUMMARY  total_diff_samples=%lld max_ulp=%d policy(max_ulp<=%d)=%s\n",
                totalDiffs, totalMaxUlp, maxUlp,
                policyOk ? "OK" : "VIOLATED");
    if (maxDiffSamples >= 0 && totalDiffs > maxDiffSamples) {
        std::printf("FAIL  diff samples %lld exceed cap %lld\n", totalDiffs,
                    maxDiffSamples);
        ++failures;
    }
    require(policyOk, "all diffs within ULP policy");
    if (failures == 0) std::printf("PASS  ring_enhancer_parity_test\n");
    return failures == 0 ? 0 : 1;
}
