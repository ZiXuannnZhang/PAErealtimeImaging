#include "RingFractionalDelay.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace ring_fbp_stream {
namespace {
using Complex = std::complex<double>;
constexpr double pi = 3.14159265358979323846;

size_t fftLength(size_t samples)
{
    if (samples > std::numeric_limits<size_t>::max() - 64)
        throw std::length_error("Fractional delay input length overflow");
    const size_t required = samples + 64;
    size_t n = 1;
    while (n < required) {
        if (n > std::numeric_limits<size_t>::max() / 2)
            throw std::length_error("Fractional delay FFT length overflow");
        n *= 2;
    }
    return n;
}
} // namespace

struct FractionalDelay::Impl {
    size_t n;
    double phaseLate = 0;
    bool phaseReady = false;
    std::vector<size_t> reverse;
    std::vector<Complex> forwardRoots, inverseRoots, phase, work;

    explicit Impl(size_t length)
        : n(length), reverse(n), forwardRoots(n - 1), inverseRoots(n - 1),
          phase(n), work(n)
    {
        for (size_t i = 1; i < n; ++i)
            reverse[i] = (reverse[i >> 1] >> 1) | ((i & 1) * (n >> 1));
        for (size_t len = 2; len <= n; len *= 2) {
            const size_t offset = len / 2 - 1;
            const auto forwardRoot = std::polar(1., -2 * pi / len);
            const auto inverseRoot = std::polar(1., 2 * pi / len);
            Complex fw = 1., iw = 1.;
            for (size_t j = 0; j < len / 2; ++j) {
                // Use the same iterative recurrence as the original FFT;
                // direct sin/cos evaluation for each twiddle would differ.
                forwardRoots[offset + j] = fw;
                inverseRoots[offset + j] = iw;
                fw *= forwardRoot;
                iw *= inverseRoot;
            }
            if (len == n) break;
        }
    }

    void preparePhase(double late)
    {
        if (phaseReady && phaseLate == late) return;
        for (size_t i = 0; i < n; ++i) {
            // Preserve the positive-frequency Nyquist convention.
            const double f = (i <= n / 2 ? double(i) : double(i) - n) / n;
            phase[i] = std::polar(1., 2 * pi * f * late);
        }
        phaseLate = late;
        phaseReady = true;
    }

    void fft(bool inverse)
    {
        for (size_t i = 1; i < n; ++i)
            if (i < reverse[i]) std::swap(work[i], work[reverse[i]]);
        const auto& roots = inverse ? inverseRoots : forwardRoots;
        for (size_t len = 2; len <= n; len *= 2) {
            const size_t offset = len / 2 - 1;
            for (size_t i = 0; i < n; i += len)
                for (size_t j = 0; j < len / 2; ++j) {
                    const auto u = work[i + j];
                    const auto v = work[i + j + len / 2] * roots[offset + j];
                    work[i + j] = u + v;
                    work[i + j + len / 2] = u - v;
                }
            if (len == n) break;
        }
        if (inverse)
            for (auto& v : work) v /= double(n);
    }
};

FractionalDelay::FractionalDelay() = default;
FractionalDelay::~FractionalDelay() = default;
FractionalDelay::FractionalDelay(FractionalDelay&&) noexcept = default;
FractionalDelay& FractionalDelay::operator=(FractionalDelay&&) noexcept = default;

void FractionalDelay::apply(std::vector<double>& line, double late)
{
    if (late == 0) return;
    const size_t n = fftLength(line.size());
    // One cached length and one cached phase only. Prepare before replacement
    // so allocation failure leaves both the input and prior plan intact.
    if (!m || m->n != n) m = std::make_unique<Impl>(n);
    m->preparePhase(late);
    std::fill(m->work.begin(), m->work.end(), Complex{});
    for (size_t i = 0; i < line.size(); ++i) m->work[i] = line[i];
    m->fft(false);
    for (size_t i = 0; i < n; ++i) m->work[i] *= m->phase[i];
    m->fft(true);
    for (size_t i = 0; i < line.size(); ++i) line[i] = float(m->work[i].real());
}

void FractionalDelay::reset() noexcept { m.reset(); }

} // namespace ring_fbp_stream
