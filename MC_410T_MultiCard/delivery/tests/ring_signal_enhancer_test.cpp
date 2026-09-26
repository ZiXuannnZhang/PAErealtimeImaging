#include "RingSignalEnhancer.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

int failures = 0;

void require(bool ok, const char *message) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL  " << message << "\n";
        throw std::runtime_error(message);
    }
}

void testValidation() {
    RingEnhanceConfig cfg;
    const char *error = nullptr;
    require(ring_enhance::validate(cfg, &error), "defaults are valid");
    cfg.freqCompFcMhz = 0.0;
    require(!ring_enhance::validate(cfg, &error), "zero cutoff is rejected");
    cfg.freqCompFcMhz = 10.0;
    cfg.freqCompHmax = 0.5;
    require(!ring_enhance::validate(cfg, &error), "Hmax below 1 is rejected");
    cfg.freqCompHmax = 2.0;
    cfg.freqCompOrder = -1.0;
    require(!ring_enhance::validate(cfg, &error), "negative order is rejected");
    std::cout << "PASS  validation\n";
}

void testFrequencyGain() {
    RingEnhanceConfig cfg;
    cfg.freqCompFcMhz = 10.0;
    cfg.freqCompHmax = 2.0;
    cfg.freqCompOrder = 2.0;
    require(std::abs(ring_enhance::frequencyGain(0.0, cfg) - 2.0) < 1e-12,
            "H(0) equals Hmax");
    double previous = ring_enhance::frequencyGain(0.0, cfg);
    for (double f = 1e6; f <= 100e6; f += 1e6) {
        const double current = ring_enhance::frequencyGain(f, cfg);
        require(current <= previous + 1e-12, "H(f) is monotonically decreasing");
        require(current >= 1.0, "H(f) remains at least 1");
        previous = current;
    }
    require(std::abs(ring_enhance::frequencyGain(100e6, cfg) - 1.0) < 1e-12,
            "H(f) tends to 1");
    std::cout << "PASS  frequency gain curve\n";
}

void testDisabledIsIdentity() {
    RingSignalEnhancer enhancer;
    RingEnhanceConfig cfg;
    enhancer.setConfig(cfg, 200e6);
    std::vector<float> input{1.0f, -2.0f, 3.5f, 0.25f};
    const auto expected = input;
    require(enhancer.apply(input.data(), static_cast<int>(input.size())),
            "disabled enhancer succeeds");
    require(input == expected, "disabled enhancer is identity");
    std::cout << "PASS  disabled identity\n";
}

void testFrequencyCompensation() {
    RingSignalEnhancer enhancer;
    RingEnhanceConfig cfg;
    cfg.enableFreqCompensation = true;
    cfg.freqCompFcMhz = 10.0;
    cfg.freqCompHmax = 2.0;
    cfg.freqCompOrder = 2.0;
    enhancer.setConfig(cfg, 200e6);
    std::vector<float> input(512, 3.25f);
    require(enhancer.apply(input.data(), static_cast<int>(input.size())),
            "frequency compensation succeeds");
    for (float value : input) {
        require(std::abs(value - 6.5f) < 1e-4f,
                "constant A-line receives the DC gain");
    }
    std::cout << "PASS  frequency compensation\n";
}

void testBipolarCompensation() {
    RingSignalEnhancer enhancer;
    RingEnhanceConfig cfg;
    cfg.enableBipolarCompensation = true;
    enhancer.setConfig(cfg, 200e6);

    std::vector<float> constant(256, 2.0f);
    require(enhancer.apply(constant.data(), static_cast<int>(constant.size())),
            "constant bipolar compensation succeeds");
    for (float value : constant) {
        require(std::abs(value - 2.0f) < 1e-6f,
                "constant signal is unchanged by derivative compensation");
    }

    std::vector<float> ramp(256);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(static_cast<double>(i) / 200e6);
    require(enhancer.apply(ramp.data(), static_cast<int>(ramp.size())),
            "ramp bipolar compensation succeeds");
    for (std::size_t i = 1; i + 1 < ramp.size(); ++i) {
        require(std::abs(ramp[i]) < 1e-7f,
                "p=t is cancelled by t*dp/dt");
    }
    std::cout << "PASS  bipolar compensation\n";
}

}  // namespace

int main() {
    try {
        testValidation();
        testFrequencyGain();
        testDisabledIsIdentity();
        testFrequencyCompensation();
        testBipolarCompensation();
    } catch (const std::exception &error) {
        std::cout << "FAIL  " << error.what() << "\n";
        return 1;
    }
    if (failures) return 1;
    std::cout << "PASS  ring_signal_enhancer_test\n";
    return 0;
}
