#pragma once
#include <string>
#include <cstddef>

struct RingTimevarLowpassConfig {
    bool enabled = false;
    double fcMaxMhz = 45.0;
    double fcMinMhz = 1.0;
    int p1 = 780;
    int p2 = 2000;
    int windowLength = 128;
    double overlapRatio = 0.75;
    int fftLength = 0;
    double filterOrder = 4.0;
    std::string transition = "smoothstep";
};

namespace ring_timevar {
// Explicit resource limits: FFT/window <= 16384, line <= 1M samples,
// cached mask <= 8M doubles (64 MiB). No parameter is silently clamped.
constexpr int maxFftLength = 16384;
constexpr int maxSamples = 1048576;
constexpr size_t maxMaskElements = 8 * 1024 * 1024;
bool validate(const RingTimevarLowpassConfig&, double fs, int samples,
              std::string* error = nullptr, bool depthKnown = true);
int fftLength(const RingTimevarLowpassConfig&);
int hop(const RingTimevarLowpassConfig&);
}
