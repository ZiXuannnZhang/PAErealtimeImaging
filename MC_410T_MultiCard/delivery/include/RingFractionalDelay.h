#pragma once
#include <memory>
#include <vector>

namespace ring_fbp_stream {

// Reusable CPU implementation of fractionalAdvance. Own one instance per
// concurrently processed stream; work buffers are deliberately not shared.
class FractionalDelay {
public:
    FractionalDelay();
    ~FractionalDelay();
    FractionalDelay(FractionalDelay&&) noexcept;
    FractionalDelay& operator=(FractionalDelay&&) noexcept;
    FractionalDelay(const FractionalDelay&) = delete;
    FractionalDelay& operator=(const FractionalDelay&) = delete;

    // Positive late advances the signal. Zero is a strict bitwise bypass.
    // Keep the reference's final float rounding even though storage is double.
    void apply(std::vector<double>& line, double late);
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ring_fbp_stream
