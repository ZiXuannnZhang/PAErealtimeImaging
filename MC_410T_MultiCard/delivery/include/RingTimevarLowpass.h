#pragma once
#include "RingTimevarLowpassConfig.h"
#include <memory>
#include <vector>
// Read-only view of the accepted CPU design, valid until reconfiguration.
// The GPU backend consumes these coefficients rather than redesigning the filter.
struct RingTimevarPlanView {
    int samples, windowLength, fftLength, hop, frames, outputLength;
    const double *window, *mask, *norm;
};
class RingTimevarLowpass {
public:
    RingTimevarLowpass();
    ~RingTimevarLowpass();
    RingTimevarLowpass(RingTimevarLowpass&&) noexcept;
    RingTimevarLowpass& operator=(RingTimevarLowpass&&) noexcept;
    // Prepare an entire immutable configuration before replacing accepted state.
    bool configure(const RingTimevarLowpassConfig&,double fs,int samples,std::string* error=nullptr);
    bool apply(std::vector<double>& line,std::string* error=nullptr);
    const RingTimevarLowpassConfig& config() const;
    size_t cachedBytes() const;
    RingTimevarPlanView planView() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
