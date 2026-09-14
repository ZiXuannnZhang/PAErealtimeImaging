#include "RingBlockAssembler.h"

#include <algorithm>
#include <cstring>
#include <iterator>

void RingBlockAssembler::configure(const int enabledChannels[8],
                                   int perChannelBlock, int sampDepth,
                                   double sectorStartDeg, double sectorWidthDeg,
                                   double stepDeg, int perChannelFrame,
                                   int triggerWlOdd, double timeoutResetSec)
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
    m_timeoutResetSec = timeoutResetSec;
    m_lastTriggerUs.store(0, std::memory_order_relaxed);
    m_configured = m_channelCount > 0 && perChannelBlock > 0 && sampDepth > 0;
    reset();
}

void RingBlockAssembler::reset()
{
    m_pending.clear();
    m_blockSeq = 0;
    m_blockTriggers.store(0, std::memory_order_relaxed);
    m_globalTrigger = 0;
    m_nextPendingOrder = 0;
    m_lastTriggerUs.store(0, std::memory_order_relaxed);
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

bool RingBlockAssembler::completeLogicalRound()
{
    if (!m_configured || !m_pending.empty() ||
        m_blockTriggers.load(std::memory_order_relaxed) != 0)
        return false;

    // The ring dialog validates that N logical triggers is an integral number
    // of blocks. Keep the block sequence monotonic, but reset the wavelength
    // and angular phase that must restart at the next physical round.
    m_globalTrigger = 0;
    m_nextPendingOrder = 0;
    return true;
}

void RingBlockAssembler::resetAfterPhysicalTimeout()
{
    resetRoundState();
    if (m_timeoutCallback)
        m_timeoutCallback();
}

void RingBlockAssembler::pushChannelLine(int channelId, uint16_t triggerSeq,
                                         const float *line, int length)
{
    if (!m_configured || channelId < 0 || channelId >= 8 ||
        !m_enabled[channelId] || !line || length <= 0)
        return;

    // 超时重置：停机超时后新到触发判定为新一圈，清空旧半块/计数并通知重建侧复位
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t lastUs = m_lastTriggerUs.load(std::memory_order_relaxed);
    if (!m_timeoutManagedExternally && m_timeoutResetSec > 0.0 && lastUs > 0) {
        const double dt = static_cast<double>(nowUs - lastUs) / 1e6;
        if (dt > m_timeoutResetSec) {
            resetAfterPhysicalTimeout();
        }
    }
    m_lastTriggerUs.store(nowUs, std::memory_order_relaxed);

    auto it = m_pending.find(triggerSeq);
    if (it == m_pending.end()) {
        PendingTrigger pending;
        pending.firstSeenOrder = m_nextPendingOrder++;
        it = m_pending.emplace(triggerSeq, std::move(pending)).first;
    }

    PendingTrigger &pt = it->second;
    if ((pt.mask & (1u << channelId)) == 0) {
        const int copyCount = std::min(length, m_sampDepth);
        // Keep every completed line exactly sampDepth samples long.  This
        // makes the later fixed-size memcpy safe for short A-lines while
        // retaining their existing completion semantics.
        std::vector<float> &saved = pt.lines[channelId];
        saved.assign(static_cast<size_t>(m_sampDepth), 0.0f);
        std::copy_n(line, copyCount, saved.begin());
        pt.mask |= (1u << channelId);
    }

    // 防止个别通道丢触发导致永久等待：最多缓冲 32 个未完成触发，超出丢最旧
    // （100Hz 触发率下 32 个 ≈ 320ms 容差，覆盖通道间到达抖动）
    while (m_pending.size() > 32) {
        auto oldest = m_pending.begin();
        for (auto candidate = std::next(m_pending.begin());
             candidate != m_pending.end(); ++candidate) {
            if (candidate->second.firstSeenOrder < oldest->second.firstSeenOrder)
                oldest = candidate;
        }
        m_pending.erase(oldest);
    }

    // Eviction may have removed the current trigger.  Re-find it instead of
    // using a reference that could have been invalidated by erase above.
    it = m_pending.find(triggerSeq);
    if (it != m_pending.end() && it->second.mask == m_allMask) {
        PendingTrigger done = std::move(it->second);
        m_pending.erase(it);
        appendCompletedTrigger(done);
    }
}

void RingBlockAssembler::resetRoundState()
{
    m_pending.clear();
    m_blockTriggers.store(0, std::memory_order_relaxed);
    m_globalTrigger = 0;
    m_nextPendingOrder = 0;
    m_lastTriggerUs.store(0, std::memory_order_relaxed);
    // 超时判定新一圈：块序号随新一圈重新计数，使“seq % 每圈块数 == 0”的
    // 圈末判定点与重建侧清零（ring_reset 后服务端从 0 计数）重新对齐
    m_blockSeq = 0;
    if (m_configured) {
        const size_t alines = static_cast<size_t>(m_channelCount) * m_perChannelBlock;
        m_raw.assign(alines * static_cast<size_t>(m_sampDepth), 0.0f);
        m_angles.assign(alines, 0.0f);
        m_channels.assign(alines, 0);
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
    if (m_progressCallback) m_progressCallback();

    if (m_blockTriggers == m_perChannelBlock && m_callback) {
        m_callback(std::move(m_raw), std::move(m_angles),
                   std::move(m_channels), m_blockSeq);
        ++m_blockSeq;
        m_blockTriggers.store(0, std::memory_order_relaxed);
        const size_t alines = static_cast<size_t>(m_channelCount) * m_perChannelBlock;
        m_raw.assign(alines * static_cast<size_t>(m_sampDepth), 0.0f);
        m_angles.assign(alines, 0.0f);
        m_channels.assign(alines, 0);
    }
}
