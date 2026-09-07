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

// =====================================================================
// RingImagingShmHeader — 环形扫描共享内存（与线性 pa_recon 分支并行）
//
// 内存布局:
//   [0, 64)                            : Header
//   [64, 64 + blockSize*4)             : Ring Block Buffer (主进程写入原始 A-line 块)
//   [64 + blockSize*4, ...)            : wl1 Frame (子进程写入)
//   [... , totalSize)                  : wl2 Frame (子进程写入)
// =====================================================================
struct RingImagingShmHeader {
    uint32_t magic;          // 0x52494E47 "RING"
    uint32_t version;        // 2：v2 新增显示快照区与强校验字段（旧布局偏移保持不变）
    uint32_t block_seq;      // 原始块序号（主进程递增）
    uint32_t frame_seq;      // 双波长帧序号（子进程递增）
    uint8_t  block_ready;    // 主→子: 1=有新原始块
    uint8_t  frame_ready;    // 子→主: 1=有新双波长帧
    uint8_t  padding[2];
    uint32_t block_size;     // 原始块 float 元素数 = alines * sampDepth
    uint32_t frame_size;     // 单波长帧 float 元素数 = nx * ny
    uint16_t nx;             // 图像宽度
    uint16_t ny;             // 图像高度
    uint32_t alines;         // 块内 A-line 数
    // ---- v2 显示快照字段（沿用原 reserved 区域，旧字段偏移不变）----
    uint16_t display_nx;         // 显示网格 dn
    uint16_t display_ny;         // = dn
    uint32_t display_frame_size; // dn*dn（单波长显示帧 float 元素数）
    uint32_t snapshot_step;      // 显示降采样步长（与接收端同公式）
    uint32_t flags;              // bit0=PNG 子进程保存可用, bit1=按圈复位启用
    uint8_t  reserved[12];
};

static_assert(sizeof(RingImagingShmHeader) == 64, "RingImagingShmHeader must be 64 bytes");

// 布局（各 Offset 相对 Header 之后的数据区起点，即代码中的 h+1）：
//   [0,64)                 Header
//   [64, 64+blockSize*4)   Raw Block（A-line 一维数组，trigger-major 排列）
//   [...]                  Angles（每根 A-line 角度，float，alines 个）
//   [...]                  Channels（每根 A-line 物理通道号，uint8，alines 个，4字节对齐）
//   [...]                  wl1 Frame
//   [...]                  wl2 Frame
// 注：方案A 下“显示图像=全分辨率重建矩阵”（每像素=gridSize，10μm），
//     显示区与全分辨率区是同一区域（ringDisplayFramesOffset == ringFramesOffset），
//     不做显示级降采样；如未来回退方案B（视图缓存），再改为附加区并同步调整偏移。
inline size_t ringAnglesOffset(int blockSize) { return static_cast<size_t>(blockSize) * sizeof(float); }
inline size_t ringChannelsOffset(int blockSize, int alines) { return ringAnglesOffset(blockSize) + static_cast<size_t>(alines) * sizeof(float); }
inline size_t ringFramesOffset(int blockSize, int alines) { return ringChannelsOffset(blockSize, alines) + ((static_cast<size_t>(alines) + 3) & ~3ULL); }
// ---- v2：显示快照区（方案A：与全分辨率帧区同一区域）----
inline size_t ringDisplayFramesOffset(int blockSize, int alines, int frameSize)
{
    (void)frameSize;
    return ringFramesOffset(blockSize, alines);   // 方案A：显示区=全分辨率区
}

inline size_t ringImagingShmTotalSizeV2(int blockSize, int frameSize, int alines,
                                        int displayFrameSize)
{
    return sizeof(RingImagingShmHeader)
         + ringDisplayFramesOffset(blockSize, alines, frameSize)
         + 2ULL * static_cast<size_t>(displayFrameSize) * sizeof(float);
}
