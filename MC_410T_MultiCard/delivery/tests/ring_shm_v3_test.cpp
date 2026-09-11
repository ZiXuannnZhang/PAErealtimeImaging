#include "ImagingSharedMemory.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
}

int main()
{
    try {
        size_t stride = 0, frameOffset = 0, total = 0;
        require(ringV3ComputeLayout(50, 8, 7500, 256 * 256,
                                     stride, frameOffset, total), "valid layout");
        require(sizeof(RingImagingShmV3Header) == 128 &&
                sizeof(RingImagingShmV3SlotHeader) == 96, "fixed header sizes");
        require(stride > sizeof(RingImagingShmV3SlotHeader) &&
                frameOffset > sizeof(RingImagingShmV3Header) && total > frameOffset,
                "monotonic layout offsets");
        require(!ringV3ComputeLayout(0, 8, 7500, 1, stride, frameOffset, total),
                "zero block rejected");
        require(!ringV3ComputeLayout(51, 8, 7500, 1, stride, frameOffset, total),
                "oversized block rejected");
        require(!ringV3ComputeLayout(50, 8, std::numeric_limits<int>::max(), 1,
                                     stride, frameOffset, total), "overflow rejected");
        RingImagingShmV3Header header{};
        header.header_bytes = sizeof(RingImagingShmV3Header);
        header.slot_count = 2;
        header.slot_header_bytes = sizeof(RingImagingShmV3SlotHeader);
        header.slot_stride_bytes = static_cast<uint32_t>(stride);
        header.raw_float_count = 50u * 8u * 4u;
        header.angle_count = 50u * 8u;
        header.channel_bytes = header.angle_count;
        require(ringV3SlotHeaderOffset(1) == 224, "slot header offset");
        require(ringV3AnglesOffset(header, 0) == ringV3RawOffset(header, 0) +
                    static_cast<size_t>(header.raw_float_count) * sizeof(float),
                "slot payload offsets");
        std::cout << "PASS Ring IPC v3: fixed ABI, checked layout, overflow rejection, slot offsets\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL Ring IPC v3: " << error.what() << '\n';
        return 1;
    }
}
