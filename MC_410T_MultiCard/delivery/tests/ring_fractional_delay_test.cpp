#include "RingFractionalDelay.h"
#include "RingFbpStream.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void need(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

std::vector<double> signal(size_t n, int kind = 0)
{
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i) {
        if (kind == 1) x[i] = i == n / 3 ? 1. : 0.;
        else if (kind == 2) x[i] = i % 2 ? -1. : 1.;
        else x[i] = std::sin(i * .071) + .4 * std::cos(i * 1.7) + .002 * (i % 31);
    }
    return x;
}

double worst = 0;
int cases = 0, bitwise = 0;
void compare(const std::vector<double>& a, const std::vector<double>& b)
{
    need(a.size() == b.size(), "length mismatch");
    double err = 0, ref = 0, peak = 0, scale = 1;
    for (size_t i = 0; i < a.size(); ++i) {
        need(std::isfinite(a[i]) && std::isfinite(b[i]), "nonfinite output");
        const double d = a[i] - b[i];
        err += d * d; ref += b[i] * b[i];
        peak = std::max(peak, std::abs(d)); scale = std::max(scale, std::abs(b[i]));
    }
    worst = std::max(worst, peak / scale);
    if (!(peak / scale < 1e-6 && std::sqrt(err / std::max(ref, 1.)) < 1e-6)) {
        std::cerr << "case=" << cases << " size=" << a.size()
                  << " peak=" << peak << " normError=" << std::sqrt(err)
                  << " normReference=" << std::sqrt(ref) << '\n';
        throw std::runtime_error("fractional-delay numerical tolerance exceeded");
    }
    ++cases;
    if (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0) ++bitwise;
}

// Independent O(N^2) DFT oracle, using long-double arithmetic and no FFT
// recurrence or production plan. The positive Nyquist bin is intentional.
std::vector<double> direct(const std::vector<double>& x, double late)
{
    using Z = std::complex<long double>;
    const long double pi = std::acos(-1.L);
    size_t n = 1; while (n < x.size() + 64) n *= 2;
    std::vector<Z> spectrum(n);
    for (size_t k = 0; k < n; ++k) {
        for (size_t t = 0; t < x.size(); ++t)
            spectrum[k] += static_cast<long double>(x[t]) *
                std::polar(1.L, -2 * pi * k * t / n);
        const long double f = (k <= n / 2 ? static_cast<long double>(k) :
            static_cast<long double>(k) - n) / n;
        spectrum[k] *= std::polar(1.L, 2 * pi * f * late);
    }
    std::vector<double> out(x.size());
    for (size_t t = 0; t < x.size(); ++t) {
        Z v{};
        for (size_t k = 0; k < n; ++k)
            v += spectrum[k] * std::polar(1.L, 2 * pi * k * t / n);
        out[t] = float(v.real() / n);
    }
    return out;
}

void checkLegacy(ring_fbp_stream::FractionalDelay& filter, size_t n, double late, int kind = 0)
{
    auto actual = signal(n, kind), expected = actual;
    ring_fbp_stream::fractionalAdvance(expected, late);
    filter.apply(actual, late);
    compare(actual, expected);
}
}

int main()
{
    try {
        ring_fbp_stream::FractionalDelay filter;
        std::vector<double> bypass = {-0., 1.2345678912345,
            std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()};
        const auto bytes = bypass;
        filter.apply(bypass, 0.);
        filter.apply(bypass, -0.);
        need(std::memcmp(bypass.data(), bytes.data(), bytes.size() * sizeof(double)) == 0,
             "zero delay is not a strict bitwise bypass");

        for (size_t n : {size_t(0), size_t(1), size_t(17), size_t(64), size_t(128), size_t(257),
                         size_t(4000), size_t(7500), size_t(8192)})
            for (double late : {0., 1., -2., .25, -1.375, 12.125})
                checkLegacy(filter, n, late);
        for (int kind : {0, 1, 2})
            for (size_t n : {size_t(3), size_t(31), size_t(64)})
                for (double late : {1., -2., .25, -1.375}) {
                    auto x = signal(n, kind), expected = direct(x, late);
                    filter.apply(x, late); compare(x, expected);
                }

        // Different raw lengths can share one FFT length; never retain a tail.
        for (size_t n : {size_t(7500), size_t(7053), size_t(7500), size_t(17), size_t(7500)})
            checkLegacy(filter, n, .25);
        for (double late : {.25, .25, -.25, 0., 3., .25}) checkLegacy(filter, 7500, late);
        ring_fbp_stream::FractionalDelay other;
        for (int i = 0; i < 3; ++i) {
            checkLegacy(filter, 7500, .25, 1);
            checkLegacy(other, 257, -1.375, 2);
        }
        auto moved = std::move(filter);
        checkLegacy(moved, 7500, .25);
        checkLegacy(filter, 128, -2.); // moved-from objects are reusable
        other = std::move(moved);
        checkLegacy(other, 7500, -.25);
        other.reset();
        checkLegacy(other, 7500, -.25);
        other.apply(bypass, 0.);
        need(std::memcmp(bypass.data(), bytes.data(), bytes.size() * sizeof(double)) == 0,
             "warm zero delay changed input");
        std::cout << "PASS cases=" << cases << " bitwise=" << bitwise
                  << " worstNormalizedAbs=" << worst << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
