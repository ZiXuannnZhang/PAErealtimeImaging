#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

// =====================================================================
// RingBlockAssembler — 环形扫描真实采集组包器（阶段 B）
//
// 输入：每个物理通道（0..7）按触发到达的一根 A-line
//       （float32，sampDepth 点，频率数据，与线性实例 DataProcessor 输出一致）。
// 输出：当所有启用通道的同一个 triggerSeq 均到达后，把各通道 A-line 按
//       trigger-major 顺序组装进块缓冲；每通道累计 perChannelBlock 根后
//       通过回调提交一个环形块（raw + 每根角度 + 通道号 + 块序号）。
//
// 角度约定（与 MATLAB 参考/ImagingSvc 一致）：
//   - 通道 s（启用序 0..M-1）扇区起点 = sectorStartDeg + s*sectorWidthDeg；
//   - 全局触发 g（0 起）：g 偶 = wl1，g 奇 = wl2（triggerWlOdd=1）；
//   - wl1 行 k=g/2 的角度 = 扇区起点 + k*stepDeg；
//   - wl2 原始行 k=g/2 的角度 = 扇区起点 + (k+1)*stepDeg（跨块对齐后与 wl1 同角）；
//   - 连续多圈采集时角度按每圈 K 回绕（首圈与单圈参考一致）。
// =====================================================================
class RingBlockAssembler
{
public:
    using BlockCallback = std::function<void(std::vector<float> &&raw,
                                             std::vector<float> &&anglesDeg,
                                             std::vector<uint8_t> &&channels,
                                             int blockSeq)>;

    // enabledChannels: 8 个物理通道是否启用；stepDeg = sectorWidth / K
    // （K = 每通道每波长每圈 A-line 数）。
    void configure(const int enabledChannels[8], int perChannelBlock,
                   int sampDepth, double sectorStartDeg, double sectorWidthDeg,
                   double stepDeg, int perChannelFrame, int triggerWlOdd);

    void reset();

    // 一个物理通道在某触发的一根 A-line（取前 sampDepth 点）。
    // 所有启用通道同一 triggerSeq 到达后立即按触发顺序组块。
    void pushChannelLine(int channelId, uint16_t triggerSeq,
                         const float *line, int length);

    void setBlockCallback(BlockCallback cb) { m_callback = std::move(cb); }

private:
    struct PendingTrigger {
        std::array<std::vector<float>, 8> lines;
        uint32_t mask = 0;
    };

    void appendCompletedTrigger(const PendingTrigger &pt);

    BlockCallback m_callback;
    bool m_configured = false;
    int  m_enabled[8] = {0};
    int  m_physOfSel[8] = {-1};
    int  m_channelCount = 0;
    int  m_perChannelBlock = 0;
    int  m_sampDepth = 0;
    double m_sectorStartDeg = 0.0;
    double m_sectorWidthDeg = 0.0;
    double m_stepDeg = 0.0;
    int  m_perChannelFrame = 0;   // 每通道每波长每圈 A-line 数（角度按圈回绕）
    int  m_triggerWlOdd = 1;

    int m_blockSeq = 0;
    int m_blockTriggers = 0;
    uint64_t m_globalTrigger = 0;
    uint32_t m_allMask = 0;
    std::map<uint16_t, PendingTrigger> m_pending;

    std::vector<float> m_raw;
    std::vector<float> m_angles;
    std::vector<uint8_t> m_channels;
};
