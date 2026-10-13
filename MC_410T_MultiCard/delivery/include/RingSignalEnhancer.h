#pragma once

#include <memory>

#include "RingEnhanceConfig.h"

class RingSignalEnhancer {
public:
    RingSignalEnhancer();
    ~RingSignalEnhancer();

    void setConfig(const RingEnhanceConfig &config, double sampleRateHz);
    const RingEnhanceConfig &config() const;
    bool enabled() const;
    bool apply(float *samples, int sampleCount);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
