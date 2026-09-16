#include "RingBlockAssembler.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace {
constexpr std::size_t kMaxPendingTriggers = 32;

bool hasResidual(std::size_t pendingCount, int blockTriggers)
{
    return pendingCount != 0 || blockTriggers != 0;
}
}

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
    m_activeRound.reset();
    m_minimumRound.reset();
    m_requireNewIdentityAfterTimeout = false;
    m_blockRound.reset();
    m_lastReportedStaleRound.reset();
    m_cleanTransitions = 0;
    m_residualTransitions = 0;
    m_residualPendingTriggers = 0;
    m_residualBlockTriggers = 0;
    m_staleRoundDrops = 0;
    m_generationGapTransitions = 0;
    m_invalidIdentityDrops = 0;
    m_blockIdentityViolations = 0;
    m_pendingEvictions = 0;
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

void RingBlockAssembler::beginMeasurementSession(std::uint64_t measurementSession)
{
    reset();
    if (measurementSession != 0)
        m_minimumRound = paimage::RoundIdentity{measurementSession, 0};
}

bool RingBlockAssembler::completeLogicalRound()
{
    if (!m_configured || !m_pending.empty() ||
        m_blockTriggers.load(std::memory_order_relaxed) != 0)
        return false;

    // Compatibility seam only. Production phase changes are driven by the
    // next identity-bearing data input, not by a CountBoundary observer.
    m_globalTrigger = 0;
    m_nextPendingOrder = 0;
    m_blockRound.reset();
    return true;
}

void RingBlockAssembler::resetAfterPhysicalTimeout()
{
    resetRoundState(true);
    if (m_timeoutCallback)
        m_timeoutCallback();
}

void RingBlockAssembler::resetRoundState(bool requireNewIdentity)
{
    resetPhaseAndResidual(true);
    // Preserve the existing timeout contract: the service-side ring reset
    // starts a new block sequence epoch.  This sequence is diagnostic only;
    // RoundIdentity remains the sole physical-round ownership key.
    m_blockSeq = 0;
    m_lastTriggerUs.store(0, std::memory_order_relaxed);
    if (requireNewIdentity && m_activeRound.has_value()) {
        // Keep the old active identity as a floor. The next physical round
        // must arrive with a strictly newer identity; late old data cannot
        // silently become the first line of a post-timeout round.
        m_requireNewIdentityAfterTimeout = true;
    } else {
        m_activeRound.reset();
        m_requireNewIdentityAfterTimeout = false;
    }
}

void RingBlockAssembler::resetPhaseAndResidual(bool countResidual)
{
    const std::size_t pendingCount = m_pending.size();
    const int blockTriggers = m_blockTriggers.load(std::memory_order_relaxed);
    if (countResidual && hasResidual(pendingCount, blockTriggers)) {
        ++m_residualTransitions;
        m_residualPendingTriggers += pendingCount;
        m_residualBlockTriggers += static_cast<std::uint64_t>(blockTriggers);
    }
    m_pending.clear();
    m_blockTriggers.store(0, std::memory_order_relaxed);
    m_globalTrigger = 0;
    m_nextPendingOrder = 0;
    m_blockRound.reset();
    if (m_configured) {
        const size_t alines = static_cast<size_t>(m_channelCount) * m_perChannelBlock;
        m_raw.assign(alines * static_cast<size_t>(m_sampDepth), 0.0f);
        m_angles.assign(alines, 0.0f);
        m_channels.assign(alines, 0);
    }
}

void RingBlockAssembler::notifyRoundTransition(const RoundTransition &event) const
{
    if (m_roundTransitionCallback)
        m_roundTransitionCallback(event);
}

bool RingBlockAssembler::acceptRound(const paimage::RoundIdentity &round)
{
    if (!round.valid()) {
        ++m_invalidIdentityDrops;
        RoundTransition event;
        event.kind = RoundTransition::Kind::InvalidIdentity;
        event.incomingRound = round;
        notifyRoundTransition(event);
        return false;
    }

    if (m_minimumRound.has_value() && round < *m_minimumRound) {
        ++m_staleRoundDrops;
        if (!m_lastReportedStaleRound.has_value() ||
            *m_lastReportedStaleRound != round) {
            m_lastReportedStaleRound = round;
            RoundTransition event;
            event.kind = RoundTransition::Kind::StaleDrop;
            event.oldRound = m_minimumRound.value();
            event.incomingRound = round;
            notifyRoundTransition(event);
        }
        return false;
    }

    if (!m_activeRound.has_value()) {
        m_activeRound = round;
        m_requireNewIdentityAfterTimeout = false;
        RoundTransition event;
        event.kind = RoundTransition::Kind::FirstRound;
        event.incomingRound = round;
        notifyRoundTransition(event);
        return true;
    }

    const paimage::RoundIdentity active = *m_activeRound;
    if (m_requireNewIdentityAfterTimeout &&
        (round < active || round == active)) {
        ++m_staleRoundDrops;
        if (!m_lastReportedStaleRound.has_value() ||
            *m_lastReportedStaleRound != round) {
            m_lastReportedStaleRound = round;
            RoundTransition event;
            event.kind = RoundTransition::Kind::StaleDrop;
            event.oldRound = active;
            event.incomingRound = round;
            event.pendingCount = m_pending.size();
            event.blockTriggers = m_blockTriggers.load(std::memory_order_relaxed);
            notifyRoundTransition(event);
        }
        return false;
    }

    if (round == active)
        return true;

    if (round < active) {
        ++m_staleRoundDrops;
        if (!m_lastReportedStaleRound.has_value() ||
            *m_lastReportedStaleRound != round) {
            m_lastReportedStaleRound = round;
            RoundTransition event;
            event.kind = RoundTransition::Kind::StaleDrop;
            event.oldRound = active;
            event.incomingRound = round;
            event.pendingCount = m_pending.size();
            event.blockTriggers = m_blockTriggers.load(std::memory_order_relaxed);
            notifyRoundTransition(event);
        }
        return false;
    }

    const std::size_t pendingCount = m_pending.size();
    const int blockTriggers = m_blockTriggers.load(std::memory_order_relaxed);
    const bool residual = hasResidual(pendingCount, blockTriggers);
    const bool generationGap = active.measurementSession == round.measurementSession &&
        round.roundGeneration > active.roundGeneration &&
        round.roundGeneration - active.roundGeneration > 1;

    if (generationGap)
        ++m_generationGapTransitions;

    RoundTransition event;
    event.kind = generationGap ? RoundTransition::Kind::GenerationGap
                               : (residual ? RoundTransition::Kind::ResidualDiscard
                                           : RoundTransition::Kind::Clean);
    event.oldRound = active;
    event.incomingRound = round;
    event.pendingCount = pendingCount;
    event.blockTriggers = blockTriggers;
    event.discardedPendingTriggers = residual ? pendingCount : 0;
    event.discardedBlockTriggers = residual
        ? static_cast<std::uint64_t>(blockTriggers) : 0;

    // Data-plane hard barrier: clear the old phase before accepting the first
    // line of the incoming identity. OLD residual can never be completed by
    // NEW data.
    resetPhaseAndResidual(false);
    if (residual) {
        ++m_residualTransitions;
        m_residualPendingTriggers += pendingCount;
        m_residualBlockTriggers += static_cast<std::uint64_t>(blockTriggers);
    } else {
        ++m_cleanTransitions;
    }
    m_activeRound = round;
    m_requireNewIdentityAfterTimeout = false;
    notifyRoundTransition(event);
    return true;
}

void RingBlockAssembler::pushChannelLine(int channelId, uint16_t triggerSeq,
                                         const paimage::RoundIdentity &round,
                                         const float *line, int length, bool roundComplete)
{
    if (!m_configured || channelId < 0 || channelId >= 8 ||
        !m_enabled[channelId] || !line || length <= 0)
        return;

    // Standalone timer ownership is separate from the production normalizer.
    // It has no next identity to compare, so it starts a fresh phase directly.
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t lastUs = m_lastTriggerUs.load(std::memory_order_relaxed);
    if (!m_timeoutManagedExternally && m_timeoutResetSec > 0.0 && lastUs > 0) {
        const double dt = static_cast<double>(nowUs - lastUs) / 1e6;
        if (dt > m_timeoutResetSec) {
            resetRoundState(false);
            if (m_timeoutCallback)
                m_timeoutCallback();
        }
    }
    if (!acceptRound(round))
        return;
    m_lastTriggerUs.store(nowUs, std::memory_order_relaxed);

    auto it = m_pending.find(triggerSeq);
    if (it == m_pending.end()) {
        PendingTrigger pending;
        pending.round = round;
        pending.firstSeenOrder = m_nextPendingOrder++;
        it = m_pending.emplace(triggerSeq, std::move(pending)).first;
    }

    PendingTrigger &pt = it->second;
    pt.roundComplete = pt.roundComplete || roundComplete;
    if (pt.round != round) {
        ++m_blockIdentityViolations;
        resetPhaseAndResidual(false);
        return;
    }
    if ((pt.mask & (1u << channelId)) == 0) {
        const int copyCount = std::min(length, m_sampDepth);
        std::vector<float> &saved = pt.lines[channelId];
        saved.assign(static_cast<size_t>(m_sampDepth), 0.0f);
        std::copy_n(line, copyCount, saved.begin());
        pt.mask |= (1u << channelId);
    }

    // 防止个别通道丢触发导致永久等待：最多缓冲 32 个未完成触发。
    while (m_pending.size() > kMaxPendingTriggers) {
        auto oldest = m_pending.begin();
        for (auto candidate = std::next(m_pending.begin());
             candidate != m_pending.end(); ++candidate) {
            if (candidate->second.firstSeenOrder < oldest->second.firstSeenOrder)
                oldest = candidate;
        }
        m_pending.erase(oldest);
        ++m_pendingEvictions;
    }

    // Eviction may have removed the current trigger. Re-find it instead of
    // using a reference that could have been invalidated by erase above.
    it = m_pending.find(triggerSeq);
    if (it != m_pending.end() && it->second.mask == m_allMask) {
        PendingTrigger done = std::move(it->second);
        m_pending.erase(it);
        appendCompletedTrigger(done);
    }
}

void RingBlockAssembler::pushChannelLine(int channelId, uint16_t triggerSeq,
                                         const float *line, int length)
{
    // Historical standalone fixture seam. It still supplies an explicit
    // identity; production Ring feeding uses the identity-bearing overload.
    pushChannelLine(channelId, triggerSeq,
                    paimage::RoundIdentity{1, 0}, line, length);
}

void RingBlockAssembler::appendCompletedTrigger(const PendingTrigger &pt)
{
    if (!m_activeRound.has_value() || pt.round != *m_activeRound) {
        ++m_blockIdentityViolations;
        resetPhaseAndResidual(false);
        return;
    }
    if (!m_blockRound.has_value())
        m_blockRound = pt.round;
    if (*m_blockRound != pt.round) {
        ++m_blockIdentityViolations;
        resetPhaseAndResidual(false);
        return;
    }

    const uint64_t g = m_globalTrigger++;
    const bool isWl1 = (m_triggerWlOdd ? (g % 2 == 0) : (g % 2 == 1));
    const int kRaw = static_cast<int>(g / 2);
    const int k = (m_perChannelFrame > 0) ? (kRaw % m_perChannelFrame) : kRaw;
    const int kNext = (m_perChannelFrame > 0 && kRaw >= m_perChannelFrame)
                      ? ((kRaw + 1) % m_perChannelFrame) : (kRaw + 1);
    const int base = m_blockTriggers.load(std::memory_order_relaxed) * m_channelCount;

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
    m_blockTriggers.fetch_add(1, std::memory_order_relaxed);
    if (m_progressCallback) m_progressCallback();

    if (m_blockTriggers.load(std::memory_order_relaxed) == m_perChannelBlock) {
        const paimage::RoundIdentity blockRound = *m_blockRound;
        if (m_callback)
            m_callback(std::move(m_raw), std::move(m_angles),
                       std::move(m_channels), m_blockSeq, blockRound, pt.roundComplete);
        ++m_blockSeq;
        m_blockTriggers.store(0, std::memory_order_relaxed);
        m_blockRound.reset();
        const size_t alines = static_cast<size_t>(m_channelCount) * m_perChannelBlock;
        m_raw.assign(alines * static_cast<size_t>(m_sampDepth), 0.0f);
        m_angles.assign(alines, 0.0f);
        m_channels.assign(alines, 0);
    }
    if (pt.roundComplete) {
        // A final trigger in a partial block cannot produce a complete image.
        // Close this identity so late channels cannot complete the residual.
        resetPhaseAndResidual(true);
        m_requireNewIdentityAfterTimeout = true;
    }
}

RingBlockAssembler::Snapshot RingBlockAssembler::snapshot() const
{
    Snapshot result;
    result.hasActiveRound = m_activeRound.has_value();
    if (result.hasActiveRound) result.activeRound = *m_activeRound;
    result.hasMinimumRound = m_minimumRound.has_value();
    if (result.hasMinimumRound) result.minimumRound = *m_minimumRound;
    result.pendingCount = m_pending.size();
    result.blockTriggers = m_blockTriggers.load(std::memory_order_relaxed);
    result.cleanTransitions = m_cleanTransitions;
    result.residualTransitions = m_residualTransitions;
    result.residualPendingTriggers = m_residualPendingTriggers;
    result.residualBlockTriggers = m_residualBlockTriggers;
    result.staleRoundDrops = m_staleRoundDrops;
    result.generationGapTransitions = m_generationGapTransitions;
    result.invalidIdentityDrops = m_invalidIdentityDrops;
    result.blockIdentityViolations = m_blockIdentityViolations;
    result.pendingEvictions = m_pendingEvictions;
    return result;
}
