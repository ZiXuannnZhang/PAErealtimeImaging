#pragma once
#include "RingTimevarLowpassConfig.h"
#include <memory>
#include <vector>
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
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
