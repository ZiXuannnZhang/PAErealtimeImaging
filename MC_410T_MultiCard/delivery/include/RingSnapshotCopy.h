#pragma once
#include "ImagingSharedMemory.h"
#include <cstdint>
#include <cstring>
#include <limits>

namespace ring_snapshot {
enum class CopyResult { Copied, LockFailed, MissingData, SequenceMismatch, InvalidBounds };

struct Layout {
    std::size_t frameOffset = 0; // absolute byte offset, including the header
    std::size_t frameBytes = 0;  // both wavelengths
    std::size_t mappingBytes = 0;
};

inline std::size_t checkedSquareFrameSize(std::size_t nx) {
    constexpr auto maxPixels = (std::numeric_limits<int>::max)();
    if (!nx || nx > (std::numeric_limits<std::uint16_t>::max)() || nx > maxPixels / nx)
        return 0;
    return nx * nx;
}

inline bool checkedLayout(std::size_t blockSize, std::size_t alines,
                          std::size_t frameSize, Layout &layout) {
    constexpr auto max = (std::numeric_limits<std::size_t>::max)();
    std::size_t end = sizeof(RingImagingShmHeader);
    auto append = [&](std::size_t count, std::size_t width) {
        if (count > (max - end) / width) return false;
        end += count * width;
        return true;
    };
    if (!append(blockSize, sizeof(float)) || !append(alines, sizeof(float)) ||
        alines > max - 3 || !append((alines + 3) & ~std::size_t(3), 1)) return false;
    const auto frameOffset = end;
    if (!frameSize || !append(frameSize, 2 * sizeof(float))) return false;
    const auto frameBytes = end - frameOffset;
    if (end > max - 7) return false;
    end = (end + 7) & ~std::size_t(7);
    if (!append(1, sizeof(RingInputMetadata))) return false;
    layout = {frameOffset, frameBytes, end};
    return true;
}

// Sequence validation and pixel reads are one shared-memory transaction.
template<class SharedMemory>
CopyResult copy(SharedMemory &shm, std::uint32_t expected, const Layout &layout,
                std::size_t capacity, float *destination, std::size_t destinationFloats,
                std::uint32_t &observed) {
    if (!destination || layout.frameOffset < sizeof(RingImagingShmHeader) ||
        layout.frameOffset > capacity || !layout.frameBytes ||
        layout.frameBytes > capacity - layout.frameOffset ||
        layout.frameBytes % sizeof(float) ||
        layout.frameBytes / sizeof(float) > destinationFloats)
        return CopyResult::InvalidBounds;
    if (!shm.lock()) return CopyResult::LockFailed;
    struct Unlock { SharedMemory &shm; ~Unlock() { shm.unlock(); } } unlock{shm};
    auto *h = static_cast<const RingImagingShmHeader *>(shm.data());
    if (!h) return CopyResult::MissingData;
    observed = h->frame_seq;
    if (observed != expected) return CopyResult::SequenceMismatch;
    const auto *pixels = reinterpret_cast<const std::uint8_t *>(h) + layout.frameOffset;
    std::memcpy(destination, pixels, layout.frameBytes);
    return CopyResult::Copied;
}
}
