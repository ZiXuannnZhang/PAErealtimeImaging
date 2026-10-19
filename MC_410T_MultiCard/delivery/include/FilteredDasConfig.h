#pragma once
#include <cstddef>
#include <string>
#include <vector>

struct FilteredDasConfig {
    bool enabled = true;
    double order = 0.5;
    double referenceMhz = 5.0;
    double passMhz = 40.0;
    double stopMhz = 60.0;
    double padFactor = 4.0;
    int batchColumns = 64;
};
namespace filtered_das {
constexpr size_t maxFftLength = 262144;
constexpr size_t maxWorkspaceBytes = 256 * 1024 * 1024;
constexpr size_t maxPlanBytes = 128 * 1024 * 1024;
bool validate(const FilteredDasConfig&, double fs, int nt, std::string* error = nullptr,
              bool depthKnown = true);
size_t fftLength(const FilteredDasConfig&, int nt);
// Matches preprocessBlock's 1-based DelayCut; no ReconDepth truncation.
bool inputLengths(int depth, bool delayCut, const int delays[8][2], const int enabled[8],
                  std::vector<int>& lengths, std::string* error = nullptr);
}
