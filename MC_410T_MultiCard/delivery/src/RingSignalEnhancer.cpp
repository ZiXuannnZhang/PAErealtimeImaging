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

    bool enabled() const {
        return config.enableBipolarCompensation ||
               config.enableFreqCompensation;
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

RingSignalEnhancer::RingSignalEnhancer()
    : impl_(std::make_unique<Impl>()) {}

RingSignalEnhancer::~RingSignalEnhancer() = default;

void RingSignalEnhancer::setConfig(const RingEnhanceConfig &config,
                                   double sampleRateHz) {
    impl_->fs = sampleRateHz;
    impl_->config = config;
    impl_->plans.clear();
}

const RingEnhanceConfig &RingSignalEnhancer::config() const {
    return impl_->config;
}

bool RingSignalEnhancer::enabled() const { return impl_->enabled(); }

bool RingSignalEnhancer::apply(float *samples, int sampleCount) {
    return impl_->apply(samples, sampleCount);
}
