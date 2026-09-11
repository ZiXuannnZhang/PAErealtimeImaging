#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>

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

// =====================================================================
// Ring IPC v3: two bounded input slots.  The v2 layout remains available for
// source compatibility, but the ring controller/service use only this ABI.
// QSharedMemory::lock() provides the inter-process critical section; state is
// a fixed-width value and never an in-process atomic or pointer.
// =====================================================================
constexpr uint32_t RING_IMAGING_SHM_V3_MAGIC = 0x52494E47u;
constexpr uint32_t RING_IMAGING_SHM_V3_VERSION = 3u;
constexpr uint32_t RING_IMAGING_SHM_V3_SLOT_COUNT = 2u;

enum class RingImagingV3SlotState : uint32_t {
    Free = 0,
    Ready = 1
};

#pragma pack(push, 1)
struct RingImagingShmV3Header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t slot_count;
    uint32_t block_capacity;
    uint32_t channel_count;
    uint32_t samp_depth;
    uint32_t alines_capacity;
    uint32_t raw_float_count;
    uint32_t angle_count;
    uint32_t channel_bytes;
    uint32_t wavelength_bytes;
    uint32_t frame_size;
    uint16_t nx;
    uint16_t ny;
    uint32_t display_frame_size;
    uint16_t display_nx;
    uint16_t display_ny;
    uint32_t slot_header_bytes;
    uint32_t slot_stride_bytes;
    uint32_t frame_offset_bytes;
    uint32_t total_bytes;
    uint32_t frame_seq;
    uint64_t service_generation;
    uint64_t config_version;
    uint64_t round_id;
    uint8_t reserved[20];
};

struct RingImagingShmV3SlotHeader {
    uint32_t state;
    uint32_t slot_index;
    uint64_t service_generation;
    uint64_t round_id;
    uint64_t config_version;
    uint64_t block_seq;
    uint64_t start_position;
    uint32_t position_count;
    uint32_t channel_count;
    uint32_t samp_depth;
    uint32_t raw_float_count;
    uint32_t raw_bytes;
    uint32_t angle_bytes;
    uint32_t channel_bytes;
    uint32_t wavelength_bytes;
    uint64_t valid_position_bits;
    uint8_t position_confidence;
    uint8_t wavelength_assumed;
    uint8_t reserved[6];
};
#pragma pack(pop)

static_assert(sizeof(RingImagingShmV3Header) == 128,
              "RingImagingShmV3Header must be 128 bytes");
static_assert(sizeof(RingImagingShmV3SlotHeader) == 96,
              "RingImagingShmV3SlotHeader must be 96 bytes");

inline bool ringV3CheckedAdd(size_t a, size_t b, size_t &out)
{
    if (b > std::numeric_limits<size_t>::max() - a) return false;
    out = a + b;
    return true;
}

inline bool ringV3CheckedMul(size_t a, size_t b, size_t &out)
{
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a) return false;
    out = a * b;
    return true;
}

inline bool ringV3ComputeLayout(int blockCapacity, int channelCount, int sampDepth,
                                int frameSize, size_t &slotStride, size_t &frameOffset,
                                size_t &totalSize)
{
    if (blockCapacity < 1 || blockCapacity > 50 || channelCount < 1 ||
        channelCount > 8 || sampDepth < 1 || frameSize < 1) return false;
    size_t alines = 0, rawFloats = 0, rawBytes = 0, angleBytes = 0;
    size_t channelBytes = 0, wavelengthBytes = 0, payload = 0;
    if (!ringV3CheckedMul(static_cast<size_t>(blockCapacity),
                          static_cast<size_t>(channelCount), alines) ||
        !ringV3CheckedMul(alines, static_cast<size_t>(sampDepth), rawFloats) ||
        !ringV3CheckedMul(rawFloats, sizeof(float), rawBytes) ||
        !ringV3CheckedMul(alines, sizeof(float), angleBytes)) return false;
    channelBytes = alines;
    wavelengthBytes = alines;
    if (!ringV3CheckedAdd(rawBytes, angleBytes, payload) ||
        !ringV3CheckedAdd(payload, channelBytes, payload) ||
        !ringV3CheckedAdd(payload, wavelengthBytes, payload) ||
        !ringV3CheckedAdd(payload, 7u, payload)) return false;
    slotStride = payload & ~static_cast<size_t>(7u);
    size_t headers = 0;
    if (!ringV3CheckedMul(RING_IMAGING_SHM_V3_SLOT_COUNT,
                          sizeof(RingImagingShmV3SlotHeader), headers) ||
        !ringV3CheckedAdd(sizeof(RingImagingShmV3Header), headers, frameOffset) ||
        !ringV3CheckedMul(RING_IMAGING_SHM_V3_SLOT_COUNT, slotStride, payload) ||
        !ringV3CheckedAdd(frameOffset, payload, frameOffset)) return false;
    size_t frames = 0;
    if (!ringV3CheckedMul(static_cast<size_t>(frameSize), sizeof(float), frames) ||
        !ringV3CheckedMul(frames, 2u, frames) ||
        !ringV3CheckedAdd(frameOffset, frames, totalSize)) return false;
    return slotStride <= std::numeric_limits<uint32_t>::max() &&
           frameOffset <= std::numeric_limits<uint32_t>::max() &&
           totalSize <= std::numeric_limits<uint32_t>::max();
}

inline size_t ringV3SlotHeaderOffset(int slot)
{
    return sizeof(RingImagingShmV3Header) +
           static_cast<size_t>(slot) * sizeof(RingImagingShmV3SlotHeader);
}

inline size_t ringV3SlotPayloadOffset(const RingImagingShmV3Header &header, int slot)
{
    return static_cast<size_t>(header.header_bytes) +
           static_cast<size_t>(header.slot_count) * header.slot_header_bytes +
           static_cast<size_t>(slot) * header.slot_stride_bytes;
}

inline size_t ringV3RawOffset(const RingImagingShmV3Header &header, int slot)
{
    return ringV3SlotPayloadOffset(header, slot);
}

inline size_t ringV3AnglesOffset(const RingImagingShmV3Header &header, int slot)
{
    return ringV3RawOffset(header, slot) +
           static_cast<size_t>(header.raw_float_count) * sizeof(float);
}

inline size_t ringV3ChannelsOffset(const RingImagingShmV3Header &header, int slot)
{
    return ringV3AnglesOffset(header, slot) +
           static_cast<size_t>(header.angle_count) * sizeof(float);
}

inline size_t ringV3WavelengthsOffset(const RingImagingShmV3Header &header, int slot)
{
    return ringV3ChannelsOffset(header, slot) + header.channel_bytes;
}

inline size_t ringV3FramesOffset(const RingImagingShmV3Header &header)
{
    return header.frame_offset_bytes;
}
