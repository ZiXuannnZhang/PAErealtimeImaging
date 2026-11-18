#pragma once

#include "RingSnapshotCopy.h"
#include "RoundIdentity.h"

// Appended to the v3 mapping when the host advertises support. Input and
// preview offsets are unchanged. Every access is under the terminal mutex.
namespace ring_terminal {
constexpr uint32_t kFlag = 1u << 2;
constexpr int kSlots = 2;
constexpr wchar_t kMutexName[] = L"Local\\MC_410T_RingShm_Terminal_v3";

struct Header {
    uint64_t submitIndex;
    paimage::RoundIdentity round;
    uint32_t seq;
    uint32_t reserved;
};
static_assert(sizeof(Header) == 32, "Terminal snapshot layout changed");

inline size_t mappingSize(const ring_snapshot::Layout &layout)
{
    constexpr auto max = (std::numeric_limits<size_t>::max)();
    if (layout.mappingBytes > max - kSlots * sizeof(Header) ||
        !layout.frameBytes || layout.frameBytes > (max - layout.mappingBytes) / kSlots - sizeof(Header))
        return 0;
    return layout.mappingBytes + kSlots * (sizeof(Header) + layout.frameBytes);
}

inline Header *slot(void *memory, size_t capacity, const ring_snapshot::Layout &layout, int i)
{
    const auto total = mappingSize(layout);
    if (!memory || !total || capacity < total || i < 0 || i >= kSlots) return nullptr;
    return reinterpret_cast<Header *>(static_cast<uint8_t *>(memory) +
        layout.mappingBytes + i * (sizeof(Header) + layout.frameBytes));
}

inline bool publish(void *memory, size_t capacity, const ring_snapshot::Layout &layout,
                    const Header &header, const float *wl1, const float *wl2)
{
    Header *available = nullptr;
    for (int i = 0; i < kSlots; ++i) {
        auto *entry = slot(memory, capacity, layout, i);
        if (!entry) return false;
        if (entry->submitIndex == header.submitIndex) return true;
        if (!entry->submitIndex && !available) available = entry;
    }
    if (!available) return false;
    auto *pixels = reinterpret_cast<uint8_t *>(available + 1);
    const auto wavelengthBytes = layout.frameBytes / 2;
    std::memcpy(pixels, wl1, wavelengthBytes);
    std::memcpy(pixels + wavelengthBytes, wl2, wavelengthBytes);
    *available = header;
    return true;
}

inline Header *oldest(void *memory, size_t capacity, const ring_snapshot::Layout &layout)
{
    Header *next = nullptr;
    for (int i = 0; i < kSlots; ++i) {
        auto *entry = slot(memory, capacity, layout, i);
        if (entry && entry->submitIndex && (!next || entry->submitIndex < next->submitIndex))
            next = entry;
    }
    return next;
}
}
