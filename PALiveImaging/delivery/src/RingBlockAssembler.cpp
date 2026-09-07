#include "RingBlockAssembler.h"

#include <algorithm>
#include <cstring>

void RingBlockAssembler::configure(const int enabledChannels[8],
                                   int perChannelBlock, int sampDepth,
                                   double sectorStartDeg, double sectorWidthDeg,
                                   double stepDeg, int perChannelFrame,
                                   int triggerWlOdd)
{
    m_channelCount = 0;
    m_allMask = 0;
    for (int c = 0; c < 8; ++c) {
        m_enabled[c] = enabledChannels[c] ? 1 : 0;
        m_physOfSel[c] = -1;
        if (m_enabled[c]) {
            m_physOfSel[m_channelCount] = c;
            m_allMask |= (1u << c);
            ++m_channelCount;
        }
    }
    m_perChannelBlock = perChannelBlock;
    m_sampDepth = sampDepth;
    m_sectorStartDeg = sectorStartDeg;
    m_sectorWidthDeg = sectorWidthDeg;
    m_stepDeg = stepDeg;
    m_perChannelFrame = perChannelFrame;
    m_triggerWlOdd = triggerWlOdd;
    m_configured = m_channelCount > 0 && perChannelBlock > 0 && sampDepth > 0;
    reset();
}

void RingBlockAssembler::reset()
{
    m_pending.clear();
    m_blockSeq = 0;
    m_blockTriggers = 0;
    m_globalTrigger = 0;
    if (m_configured) {
        const size_t alines = static_cast<size_t>(m_channelCount) * m_perChannelBlock;
        m_raw.assign(alines * static_cast<size_t>(m_sampDepth), 0.0f);
        m_angles.assign(alines, 0.0f);
        m_channels.assign(alines, 0);
    } else {
        m_raw.clear();
        m_angles.clear();
        m_channels.clear();
    }
}

void RingBlockAssembler::pushChannelLine(int channelId, uint16_t triggerSeq,
                                         const float *line, int length)
{
    if (!m_configured || channelId < 0 || channelId >= 8 ||
        !m_enabled[channelId] || !line || length <= 0)
        return;

    auto &pt = m_pending[triggerSeq];
    if ((pt.mask & (1u << channelId)) == 0) {
        pt.lines[channelId].assign(line, line + std::min(length, m_sampDepth));
        pt.mask |= (1u << channelId);
    }

    // 防止个别通道丢触发导致永久等待：最多缓冲 32 个未完成触发，超出丢最旧
    // （100Hz 触发率下 32 个 ≈ 320ms 容差，覆盖通道间到达抖动）
    while (m_pending.size() > 32)
        m_pending.erase(m_pending.begin());

    if (pt.mask == m_allMask) {
        PendingTrigger done = std::move(pt);
        m_pending.erase(triggerSeq);
        appendCompletedTrigger(done);
    }
}

void RingBlockAssembler::appendCompletedTrigger(const PendingTrigger &pt)
{
    const uint64_t g = m_globalTrigger++;
    const bool isWl1 = (m_triggerWlOdd ? (g % 2 == 0) : (g % 2 == 1));
    const int kRaw = static_cast<int>(g / 2);       // 全局每波长行号
    // 角度按圈回绕：连续多圈采集时每圈 K 根，首圈保持 kRaw/kRaw+1 与单圈参考一致
    const int k = (m_perChannelFrame > 0) ? (kRaw % m_perChannelFrame) : kRaw;
    const int kNext = (m_perChannelFrame > 0 && kRaw >= m_perChannelFrame)
                      ? ((kRaw + 1) % m_perChannelFrame) : (kRaw + 1);
    const int base = m_blockTriggers * m_channelCount;

    for (int s = 0; s < m_channelCount; ++s) {
        const int ch = m_physOfSel[s];
        std::memcpy(m_raw.data() + (static_cast<size_t>(base) + s) * m_sampDepth,
                    pt.lines[ch].data(),
                    static_cast<size_t>(m_sampDepth) * sizeof(float));
        const double angle = m_sectorStartDeg + s * m_sectorWidthDeg
                           + (isWl1 ? k : kNext) * m_stepDeg;
        m_angles[base + s] = static_cast<float>(angle);
        m_channels[base + s] = static_cast<uint8_t>(ch);
    }
    ++m_blockTriggers;

    if (m_blockTriggers == m_perChannelBlock && m_callback) {
        m_callback(std::move(m_raw), std::move(m_angles),
                   std::move(m_channels), m_blockSeq);
        ++m_blockSeq;
        m_blockTriggers = 0;
        const size_t alines = static_cast<size_t>(m_channelCount) * m_perChannelBlock;
        m_raw.assign(alines * static_cast<size_t>(m_sampDepth), 0.0f);
        m_angles.assign(alines, 0.0f);
        m_channels.assign(alines, 0);
    }
}
