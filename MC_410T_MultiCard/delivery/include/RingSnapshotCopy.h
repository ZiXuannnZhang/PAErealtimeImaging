#pragma once
#include "ImagingSharedMemory.h"
#include <cstdint>
#include <cstring>

namespace ring_snapshot {
enum class CopyResult { Copied, LockFailed, MissingData, SequenceMismatch };
// SharedMemory is QSharedMemory in production, injectable in regression tests.
// The sequence check and pixel read are one indivisible SHM transaction.
template<class SharedMemory>
CopyResult copy(SharedMemory &shm, std::uint32_t expected, int blockSize,
                int alines, std::size_t frameSize, float *destination,
                std::uint32_t &observed) {
    if (!shm.lock()) return CopyResult::LockFailed;
    struct Unlock { SharedMemory &shm; ~Unlock() { shm.unlock(); } } unlock{shm};
    auto *h = static_cast<RingImagingShmHeader *>(shm.data());
    if (!h) return CopyResult::MissingData;
    observed = h->frame_seq;
    if (observed != expected) return CopyResult::SequenceMismatch;
    const auto *pixels = reinterpret_cast<const std::uint8_t *>(h + 1)
        + ringFramesOffset(blockSize, alines);
    std::memcpy(destination, pixels, frameSize * 2 * sizeof(float));
    return CopyResult::Copied;
}
}
