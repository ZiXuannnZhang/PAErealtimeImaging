#pragma once

struct RingEnhanceConfig {
    bool enableBipolarCompensation = false;
    bool enableFreqCompensation = false;
    double freqCompFcMhz = 10.0;
    double freqCompHmax = 2.0;
    double freqCompOrder = 2.0;
};

namespace ring_enhance {

bool validate(const RingEnhanceConfig &config, const char **error = nullptr);
double frequencyGain(double frequencyHz, const RingEnhanceConfig &config);

}  // namespace ring_enhance
