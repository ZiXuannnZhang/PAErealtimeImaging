#pragma once
#include "FilteredDasConfig.h"
#include <complex>
#include <memory>

class FilteredDasPrefilter {
public:
    FilteredDasPrefilter();
    ~FilteredDasPrefilter();
    FilteredDasPrefilter(FilteredDasPrefilter&&) noexcept;
    FilteredDasPrefilter& operator=(FilteredDasPrefilter&&) noexcept;
    // Prepares every actual channel/wavelength length atomically. Disabled: no plans/workspace.
    bool configure(const FilteredDasConfig&, double fs, const std::vector<int>& lengths,
                   std::string* error = nullptr);
    bool apply(std::vector<std::vector<float>>&, std::string* error = nullptr);
    bool apply(std::vector<std::vector<double>>&, std::string* error = nullptr);
    size_t cachedBytes() const;
    size_t workspaceBytes() const;
    size_t maxBatchUsed() const;
    size_t batches() const;
    size_t transforms() const;
    // Diagnostic copy only, never used in production hot path.
    std::vector<std::complex<double>> response(int nt, bool single = false) const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
