// scratch: isolate the both-vs-freq parity contradiction (not part of the repo)
#include "RingSignalEnhancer.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace refns {
constexpr double kPi = 3.14159265358979323846;
std::size_t nextPowerOfTwo(std::size_t v) { std::size_t r = 1; while (r < v) r <<= 1; return r; }
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
            std::size_t x = i, y = 0;
            for (unsigned b = 0; b < bits; ++b) { y = (y << 1) | (x & 1u); x >>= 1; }
            bitReversal[i] = y;
        }
        twiddles.resize(fftSize / 2);
        for (std::size_t k = 0; k < twiddles.size(); ++k) {
            const double angle = -2.0 * kPi * (double)k / (double)fftSize;
            twiddles[k] = {std::cos(angle), std::sin(angle)};
        }
        freqGain.resize(fftSize);
        for (std::size_t k = 0; k < fftSize; ++k) {
            double f = (double)k * fs / (double)fftSize;
            f = std::min(f, fs - f);
            freqGain[k] = ring_enhance::frequencyGain(f, config);
        }
    }
    void transform(std::vector<std::complex<double>> &values, bool inverse) const {
        const std::size_t n = values.size();
        for (std::size_t i = 0; i < n; ++i) { const std::size_t j = bitReversal[i]; if (i < j) std::swap(values[i], values[j]); }
        for (std::size_t len = 2; len <= n; len <<= 1) {
            const std::size_t half = len >> 1, step = n / len;
            for (std::size_t base = 0; base < n; base += len)
                for (std::size_t j = 0; j < half; ++j) {
                    std::complex<double> w = twiddles[j * step];
                    if (inverse) w = std::conj(w);
                    const std::complex<double> e = values[base + j];
                    const std::complex<double> o = values[base + j + half] * w;
                    values[base + j] = e + o;
                    values[base + j + half] = e - o;
                }
        }
        if (inverse) { const double s = 1.0 / (double)n; for (auto &v : values) v *= s; }
    }
};
struct Ref {
    RingEnhanceConfig config; double fs = 1.0;
    std::unordered_map<int, FFTPlan> plans;
    bool apply(float *samples, int n) {
        if (!samples || n <= 0) return false;
        if (!config.enableBipolarCompensation && !config.enableFreqCompensation) return true;
        std::vector<double> work(samples, samples + n);
        if (config.enableFreqCompensation) applyFreq(work, n);
        if (config.enableBipolarCompensation) applyBip(work);
        for (int i = 0; i < n; ++i) samples[i] = (float)work[(size_t)i];
        return true;
    }
    void applyFreq(std::vector<double> &samples, int sampleCount) {
        const std::size_t fftSize = nextPowerOfTwo((size_t)std::max(1, sampleCount));
        auto &plan = plans[sampleCount];
        if (plan.size != fftSize || plan.freqGain.size() != fftSize) plan.build(fftSize, fs, config);
        std::vector<std::complex<double>> values(fftSize, {0.0, 0.0});
        const int extra = (int)fftSize - sampleCount;
        const int left = extra / 2;
        for (std::size_t out = 0; out < fftSize; ++out) {
            const long long src = (long long)out - (long long)left;
            values[out] = {reflect(samples, src, sampleCount), 0.0};
        }
        plan.transform(values, false);
        for (std::size_t i = 0; i < fftSize; ++i) values[i] *= plan.freqGain[i];
        plan.transform(values, true);
        for (int i = 0; i < sampleCount; ++i)
            samples[(size_t)i] = values[(size_t)(left + i)].real();
    }
    static double reflect(const std::vector<double> &s, long long index, int n) {
        if (n <= 0) return 0.0;
        if (n == 1) return s.front();
        const long long period = 2LL * (n - 1);
        long long w = index % period;
        if (w < 0) w += period;
        if (w >= n) w = period - w;
        return s[(size_t)w];
    }
    void applyBip(std::vector<double> &samples) {
        const int n = (int)samples.size();
        std::vector<double> d(samples.size(), 0.0);
        d.front() = (samples[1] - samples[0]) * fs;
        d.back() = (samples.back() - samples[samples.size() - 2]) * fs;
        for (int i = 1; i < n - 1; ++i) d[(size_t)i] = (samples[(size_t)i + 1] - samples[(size_t)i - 1]) * (fs * 0.5);
        for (int i = 0; i < n; ++i) { const double t = (double)i / fs; samples[(size_t)i] -= t * d[(size_t)i]; }
    }
};
}  // namespace refns

int ulpdist(float a, float b) {
    int ia, ib;
    std::memcpy(&ia, &a, 4); std::memcpy(&ib, &b, 4);
    if (ia < 0) ia = 0x80000000 - ia;
    if (ib < 0) ib = 0x80000000 - ib;
    return std::abs(ia - ib);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "D:/ChatGPT/PAERealtimeImaging/testdata/14.dat";
    const int len = 3830;
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("no data\n"); return 1; }
    std::vector<double> col(4000);
    f.seekg(300LL * 4000 * 8);
    f.read(reinterpret_cast<char *>(col.data()), 4000 * 8);
    std::vector<float> input(len);
    for (int i = 0; i < len; ++i) input[(size_t)i] = (float)col[(size_t)(i + 170)];

    RingEnhanceConfig cfgF, cfgB;
    cfgF.enableFreqCompensation = true;
    cfgB.enableBipolarCompensation = true;
    cfgB.enableFreqCompensation = true;
    const double fs = 250e6;

    std::vector<float> aF = input, aB = input, rF = input, rB = input, rFthenBip = input;
    RingSignalEnhancer eF, eB;
    eF.setConfig(cfgF, fs);
    eB.setConfig(cfgB, fs);
    eF.apply(aF.data(), len);
    eB.apply(aB.data(), len);
    refns::Ref r;
    r.config = cfgF; r.fs = fs;
    r.apply(rF.data(), len);
    refns::Ref r2;
    r2.config = cfgB; r2.fs = fs;
    r2.apply(rB.data(), len);
    // reference bipolar applied to the reference freq output:
    {
        std::vector<double> w(rF.size());
        for (size_t i = 0; i < w.size(); ++i) w[i] = (double)rF[i];
        refns::Ref r3;
        r3.config = cfgB; r3.fs = fs;
        r3.applyBip(w);
        for (int i = 0; i < len; ++i) rFthenBip[(size_t)i] = (float)w[(size_t)i];
    }

    long long dF = 0, dB = 0, dCross = 0, maxUF = 0, maxUB = 0;
    int firstIdx = -1;
    for (int i = 0; i < len; ++i) {
        int u;
        u = ulpdist(aF[(size_t)i], rF[(size_t)i]);
        if (u) { ++dF; maxUF = std::max(maxUF, (long long)u); }
        u = ulpdist(aB[(size_t)i], rB[(size_t)i]);
        if (u) { ++dB; maxUB = std::max(maxUB, (long long)u); if (firstIdx < 0) firstIdx = i; }
        u = ulpdist(aB[(size_t)i], rFthenBip[(size_t)i]);
        if (u) ++dCross;
    }
    std::printf("impl_freq_vs_ref_freq : diffs=%lld max_ulp=%lld\n", dF, maxUF);
    std::printf("impl_both_vs_ref_both : diffs=%lld max_ulp=%lld first=%d\n", dB, maxUB, firstIdx);
    std::printf("impl_both_vs_bip(ref_freq): diffs=%lld\n", dCross);
    if (firstIdx >= 0) {
        const int i = firstIdx;
        std::printf("at idx=%d: input=%.9g implF=%.9g refF=%.9g implB=%.9g refB=%.9g bipRefF=%.9g\n",
                    i, (double)input[(size_t)i], (double)aF[(size_t)i], (double)rF[(size_t)i],
                    (double)aB[(size_t)i], (double)rB[(size_t)i], (double)rFthenBip[(size_t)i]);
    }

    // ---- ramp case at 12330 (freq-only, hmax=2) ----
    {
        const int len = 12330;
        std::vector<float> line((size_t)len);
        for (int i = 0; i < len; ++i)
            line[(size_t)i] = (float)((double)i / 250e6);
        std::vector<float> a = line, r = line;
        RingEnhanceConfig cfg;
        cfg.enableFreqCompensation = true;
        RingSignalEnhancer e;
        e.setConfig(cfg, 250e6);
        e.apply(a.data(), len);
        refns::Ref rr;
        rr.config = cfg; rr.fs = 250e6;
        rr.apply(r.data(), len);
        long long d = 0, mu = 0; double maxAbs = 0.0; int mi = -1;
        for (int i = 0; i < len; ++i) {
            const int u = ulpdist(a[(size_t)i], r[(size_t)i]);
            if (u) {
                ++d; mu = std::max(mu, (long long)u);
                const double ad = std::fabs((double)a[(size_t)i] - (double)r[(size_t)i]);
                if (ad > maxAbs) { maxAbs = ad; mi = i; }
            }
        }
        std::printf("ramp12330_freq: diffs=%lld max_ulp=%lld max_abs=%.6g at idx=%d (ref=%.9g act=%.9g)\n",
                    d, mu, maxAbs, mi, mi >= 0 ? (double)r[(size_t)mi] : 0.0,
                    mi >= 0 ? (double)a[(size_t)mi] : 0.0);
        for (int i = 0; i < 5; ++i) {
            const int u = ulpdist(a[(size_t)i], r[(size_t)i]);
            std::printf("  idx=%d ref=%.9g act=%.9g ulp=%d\n", i, (double)r[(size_t)i],
                        (double)a[(size_t)i], u);
        }
    }

    // ---- pulse/both len=8000: print every diff beyond the class-B bounds ----
    {
        const int len = 8000;
        std::vector<float> line((size_t)len);
        for (int i = 0; i < len; ++i) {
            const double u = ((double)i / 250e6 - 0.25 * len / 250e6) / 0.2e-6;
            line[(size_t)i] = (float)(-u * std::exp(-0.5 * u * u));
        }
        std::vector<float> a = line, r = line;
        RingEnhanceConfig cfg;
        cfg.enableBipolarCompensation = true;
        cfg.enableFreqCompensation = true;
        RingSignalEnhancer e;
        e.setConfig(cfg, 250e6);
        e.apply(a.data(), len);
        refns::Ref rr;
        rr.config = cfg; rr.fs = 250e6;
        rr.apply(r.data(), len);
        long long d = 0, c = 0;
        double maxAbsAll = 0.0;
        for (int i = 0; i < len; ++i) {
            const int u = ulpdist(a[(size_t)i], r[(size_t)i]);
            if (!u) continue;
            ++d;
            const double rv = (double)r[(size_t)i], av = (double)a[(size_t)i];
            const double ad = std::fabs(rv - av);
            maxAbsAll = std::max(maxAbsAll, ad);
            const bool inB = std::max(std::fabs(rv), std::fabs(av)) <= 1e-6 && ad <= 1e-12;
            if (u > 1 && !inB) {
                ++c;
                if (c <= 15)
                    std::printf("  C-sample idx=%d ref=%.17g act=%.17g ulp=%d abs=%.6g\n",
                                i, rv, av, u, ad);
            }
        }
        std::printf("pulse8000_both: diffs=%lld beyond_classB=%lld max_abs_all=%.6g\n",
                    d, c, maxAbsAll);
    }

    // ---- bench failing assertion: constant 512, fs=250e6, freq-only ----
    for (int scenario = 0; scenario < 2; ++scenario) {
        const double hmax = scenario == 0 ? 2.0 : 20.0;
        std::vector<float> line(512, 3.25f);
        std::vector<float> a = line, r = line;
        RingEnhanceConfig cfg;
        cfg.enableFreqCompensation = true;
        cfg.freqCompHmax = hmax;
        RingSignalEnhancer e;
        e.setConfig(cfg, 250e6);
        e.apply(a.data(), 512);
        refns::Ref rr;
        rr.config = cfg; rr.fs = 250e6;
        rr.apply(r.data(), 512);
        int badA = 0, badR = 0;
        for (int i = 0; i < 512; ++i) {
            if (std::fabs((double)a[(size_t)i] - 3.25 * hmax) >= 1e-4) {
                if (badA < 5) std::printf("  const512 impl hmax=%g idx=%d val=%.9g (expect %.9g)\n",
                                          hmax, i, (double)a[(size_t)i], 3.25 * hmax);
                ++badA;
            }
            if (std::fabs((double)r[(size_t)i] - 3.25 * hmax) >= 1e-4) ++badR;
        }
        std::printf("const512 fs=250e6 hmax=%g: impl_bad=%d ref_bad=%d\n", hmax, badA, badR);
    }
    return 0;
}
