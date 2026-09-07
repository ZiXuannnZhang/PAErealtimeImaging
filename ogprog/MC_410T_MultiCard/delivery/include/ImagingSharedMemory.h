#pragma once

#include <cstdint>
#include <cstddef>

// =====================================================================
// ImagingSharedMemory — 共享内存布局定义
// 对应《成像功能扩展设计方案》v2.0 §2.3
//
// 内存布局:
//   [0, 64)                        : Header (ImagingShmHeader)
//   [64, 64 + pulseSize*4)         : Pulse Buffer  (主进程写入)
//   [64 + pulseSize*4, totalSize)  : Frame Buffer  (子进程写入)
// =====================================================================

// 共享内存 Header（64 bytes）
struct ImagingShmHeader {
    uint32_t magic;          // 0x494D4147 "IMAG"
    uint32_t version;        // 1
    uint32_t pulse_seq;      // 脉冲序号（原子递增）
    uint32_t frame_seq;      // 帧序号（原子递增）
    uint8_t  pulse_ready;    // 主→子: 1=有新脉冲
    uint8_t  frame_ready;    // 子→主: 1=有新帧
    uint8_t  padding[2];
    uint32_t pulse_size;     // 单脉冲 float 元素数 = card_num * depth
    uint32_t frame_size;     // 单帧 float 元素数 = nx * ny
    uint16_t nx;             // 图像宽度
    uint16_t ny;             // 图像高度
    uint8_t  reserved[32];
};

static_assert(sizeof(ImagingShmHeader) == 64, "ImagingShmHeader must be 64 bytes");

// 计算共享内存总大小
inline size_t imagingShmTotalSize(int pulseSize, int frameSize)
{
    return sizeof(ImagingShmHeader) + static_cast<size_t>(pulseSize + frameSize) * sizeof(float);
}
