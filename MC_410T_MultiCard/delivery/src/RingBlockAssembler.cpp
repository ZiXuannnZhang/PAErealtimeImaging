#include "RingBlockAssembler.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr std::uint8_t kUnknownWavelength = 0xff;
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
        if (m_enabled[c] && m_channelCount < 8) {
            m_physOfSel[m_channelCount] = c;
            m_allMask |= 1u << c;
            ++m_channelCount;
        }
    }
    m_perChannelBlock = std::clamp(perChannelBlock, 1, 50);
    m_sampDepth = std::max(1, sampDepth);
    m_sectorStartDeg = sectorStartDeg;
    m_sectorWidthDeg = sectorWidthDeg;
    m_stepDeg = stepDeg;
    m_perChannelFrame = std::max(0, perChannelFrame);
    m_triggerWlOdd = triggerWlOdd ? 1 : 0;
    m_timeoutResetSec = std::max(0.0, timeoutResetSec);
    m_configured = m_channelCount > 0 && perChannelBlock > 0 && sampDepth > 0;
    reset();
}

void RingBlockAssembler::setIdentityContext(std::uint64_t serviceGeneration,
                                             std::uint64_t roundId,
                                             std::uint64_t configVersion,
                                             PositionConfidence confidence)
{
    m_serviceGeneration = serviceGeneration;
    m_roundId = roundId == 0 ? 1 : roundId;
    m_configVersion = configVersion;
    m_positionConfidence = confidence;
}

std::vector<float> RingBlockAssembler::allocateRaw() const
{
    const std::size_t count = static_cast<std::size_t>(m_channelCount) *
                              static_cast<std::size_t>(m_perChannelBlock) *
                              static_cast<std::size_t>(m_sampDepth);
    return std::vector<float>(count, 0.0f);
}

void RingBlockAssembler::reset()
{
    m_pending.clear();
    m_recent.clear();
    m_blockSeq = 0;
    m_blockTriggers.store(0, std::memory_order_relaxed);
    m_nextPendingOrder = 0;
    m_nextPosition = 0;
    m_highWaterPosition = 0;
    m_blockStartPosition = 0;
    m_haveWire = false;
    m_lastWire = 0;
    m_lastExpanded = 0;
    m_lastTriggerUs.store(0, std::memory_order_relaxed);
    m_validPositionBits = 0;
    m_blockHasInvalid = false;
    m_blockHasRichInput = false;
    m_blockQualityInitialized = false;
    m_blockQuality = FrameQuality{};
    m_blockWavelengthAssumed = false;
    m_lateAfterSeal = 0;
    m_invalidPositions = 0;
    if (m_configured) {
        m_raw = allocateRaw();
        const std::size_t lines = static_cast<std::size_t>(m_channelCount) *
                                  static_cast<std::size_t>(m_perChannelBlock);
        m_angles.assign(lines, 0.0f);
        m_channels.assign(lines, 0);
        m_wavelengths.assign(lines, kUnknownWavelength);
    } else {
        m_raw.clear();
        m_angles.clear();
        m_channels.clear();
        m_wavelengths.clear();
    }
}

void RingBlockAssembler::pushChannelLine(int channelId, uint16_t triggerSeq,
                                         const float *line, int length)
{
    pushChannelLineImpl(channelId, triggerSeq, line, length, nullptr, nullptr, -1);
}

void RingBlockAssembler::pushChannelLine(int channelId, uint16_t triggerSeq,
                                         const float *line, int length,
                                         const FrameIdentity &identity,
                                         const FrameQuality &quality,
                                         int wavelength)
{
    pushChannelLineImpl(channelId, triggerSeq, line, length, &identity,
                        &quality, wavelength);
}

bool RingBlockAssembler::findRecent(uint16_t wire, std::uint64_t &position) const
{
    for (auto it = m_recent.rbegin(); it != m_recent.rend(); ++it) {
        if (it->wire == wire) {
            position = it->position;
            return true;
        }
    }
    return false;
}

void RingBlockAssembler::rememberTrigger(uint16_t wire, std::uint64_t position)
{
    m_recent.push_back({wire, position});
    while (m_recent.size() > 128) m_recent.pop_front();
}

bool RingBlockAssembler::acceptNewRound(RoundCloseReason reason, bool notify)
{
    if (!m_configured) return false;
    if (m_ringCallback) {
        publishReady(true);
        if (m_blockTriggers.load(std::memory_order_relaxed) > 0)
            emitCurrentBlock();
    }
    resetRoundState(true);
    if (notify && m_timeoutCallback) m_timeoutCallback();
    (void)reason;
    return true;
}

bool RingBlockAssembler::expandTrigger(uint16_t triggerSeq, std::uint64_t &position)
{
    if (findRecent(triggerSeq, position)) {
        if (position < m_nextPosition) ++m_lateAfterSeal;
        return position >= m_nextPosition;
    }

    if (!m_haveWire) {
        m_haveWire = true;
        m_lastWire = triggerSeq;
        m_lastExpanded = 0;
        position = 0;
        m_highWaterPosition = 0;
        rememberTrigger(triggerSeq, position);
        return true;
    }

    const std::int16_t delta = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(triggerSeq - m_lastWire));
    if (delta == std::numeric_limits<std::int16_t>::min() ||
        (delta < 0 && static_cast<std::uint32_t>(-static_cast<std::int32_t>(delta)) > 32u)) {
        acceptNewRound(delta == std::numeric_limits<std::int16_t>::min()
                           ? RoundCloseReason::HalfRangeAmbiguous
                           : RoundCloseReason::SuspectedReset,
                       true);
        m_haveWire = true;
        m_lastWire = triggerSeq;
        m_lastExpanded = 0;
        position = 0;
        m_highWaterPosition = 0;
        rememberTrigger(triggerSeq, position);
        return true;
    }

    if (delta > 0) {
        position = m_lastExpanded + static_cast<std::uint16_t>(delta);
        m_lastWire = triggerSeq;
        m_lastExpanded = position;
        m_highWaterPosition = std::max(m_highWaterPosition, position);
        rememberTrigger(triggerSeq, position);
        return position >= m_nextPosition;
    }

    const std::uint32_t magnitude = static_cast<std::uint32_t>(
        -static_cast<std::int32_t>(delta));
    if (magnitude <= m_lastExpanded && m_lastExpanded - magnitude >= m_nextPosition) {
        position = m_lastExpanded - magnitude;
        rememberTrigger(triggerSeq, position);
        return true;
    }

    ++m_lateAfterSeal;
    return false;
}

void RingBlockAssembler::pushChannelLineImpl(int channelId, uint16_t triggerSeq,
                                              const float *line, int length,
                                              const FrameIdentity *identity,
                                              const FrameQuality *quality,
                                              int wavelength)
{
    if (!m_configured || channelId < 0 || channelId >= 8 ||
        !m_enabled[channelId] || !line || length <= 0)
        return;

    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t lastUs = m_lastTriggerUs.load(std::memory_order_relaxed);
    if (m_timeoutResetSec > 0.0 && lastUs > 0 &&
        static_cast<double>(nowUs - lastUs) / 1e6 > m_timeoutResetSec)
        acceptNewRound(RoundCloseReason::IdleTimeout, true);
    m_lastTriggerUs.store(nowUs, std::memory_order_relaxed);

    std::uint64_t position = 0;
    if (!expandTrigger(triggerSeq, position)) return;
    if (position < m_nextPosition) {
        ++m_lateAfterSeal;
        return;
    }

    auto it = m_pending.find(position);
    if (it == m_pending.end()) {
        PendingTrigger pending;
        pending.firstSeenOrder = m_nextPendingOrder++;
        pending.position = position;
        it = m_pending.emplace(position, std::move(pending)).first;
    }
    PendingTrigger &pending = it->second;
    if (pending.mask & (1u << channelId)) return;

    const int copyCount = std::min(length, m_sampDepth);
    std::vector<float> &saved = pending.lines[channelId];
    saved.assign(static_cast<std::size_t>(m_sampDepth), 0.0f);
    std::copy_n(line, copyCount, saved.begin());
    pending.mask |= 1u << channelId;
    if (identity && quality) {
        m_blockHasRichInput = true;
        pending.hasRichQuality = true;
        pending.identity = *identity;
        pending.quality = *quality;
        mergeBlockQuality(*quality);
        pending.wavelengths[static_cast<std::size_t>(channelId)] =
            (wavelength == 0 || wavelength == 1)
                ? static_cast<std::uint8_t>(wavelength) : kUnknownWavelength;
        if (wavelength != 0 && wavelength != 1) m_blockWavelengthAssumed = true;
        if (!quality->inputUsable() || length != m_sampDepth)
            pending.invalidLine = true;
    }

    publishReady(false);
    while (m_pending.size() > 32) {
        auto oldest = m_pending.begin();
        if (oldest->first >= m_nextPosition + 32) {
            ++m_invalidPositions;
            m_pending.erase(oldest);
        } else {
            break;
        }
    }
    it = m_pending.find(position);
    if (it != m_pending.end() && it->second.mask == m_allMask)
        publishReady(false);
}

void RingBlockAssembler::mergeBlockQuality(const FrameQuality &quality)
{
    if (!m_blockQualityInitialized) {
        m_blockQuality = quality;
        // Packet coverage is a per-trigger detail.  Keep the aggregate
        // fixed-width block contract small while retaining the scalar result.
        m_blockQuality.packetCoverage.clear();
        m_blockQualityInitialized = true;
        return;
    }
    m_blockQuality.qualityUnknown = m_blockQuality.qualityUnknown || quality.qualityUnknown;
    m_blockQuality.assemblyComplete = m_blockQuality.assemblyComplete && quality.assemblyComplete;
    m_blockQuality.packetCoverageComplete =
        m_blockQuality.packetCoverageComplete && quality.packetCoverageComplete;
    m_blockQuality.packetLengthValid = m_blockQuality.packetLengthValid && quality.packetLengthValid;
    m_blockQuality.sampleLengthValid = m_blockQuality.sampleLengthValid && quality.sampleLengthValid;
    m_blockQuality.sampleOriginKnown = m_blockQuality.sampleOriginKnown && quality.sampleOriginKnown;
    m_blockQuality.expectedPacketCount = std::max(m_blockQuality.expectedPacketCount,
                                                  quality.expectedPacketCount);
    m_blockQuality.receivedPacketCount = std::min(m_blockQuality.receivedPacketCount,
                                                  quality.receivedPacketCount);
    m_blockQuality.expectedPayloadBytes = std::max(m_blockQuality.expectedPayloadBytes,
                                                   quality.expectedPayloadBytes);
    m_blockQuality.actualPayloadBytes = std::min(m_blockQuality.actualPayloadBytes,
                                                 quality.actualPayloadBytes);
    if (m_blockQuality.missingReason.empty() && !quality.missingReason.empty())
        m_blockQuality.missingReason = quality.missingReason;
}

void RingBlockAssembler::publishReady(bool force)
{
    while (m_nextPosition <= m_highWaterPosition) {
        auto it = m_pending.find(m_nextPosition);
        const bool complete = it != m_pending.end() && it->second.mask == m_allMask;
        const bool sealed = force || m_highWaterPosition - m_nextPosition >= 32;
        if (!complete && !sealed) break;

        const PendingTrigger *pending = it == m_pending.end() ? nullptr : &it->second;
        appendPosition(pending, m_nextPosition,
                       complete && (!pending->hasRichQuality || !pending->invalidLine));
        if (it != m_pending.end()) m_pending.erase(it);
        ++m_nextPosition;
    }
}

void RingBlockAssembler::appendPosition(const PendingTrigger *pending,
                                         std::uint64_t position, bool valid)
{
    const int slot = m_blockTriggers.load(std::memory_order_relaxed);
    if (slot == 0) m_blockStartPosition = position;
    if (slot >= m_perChannelBlock) return;

    const bool rich = pending && pending->hasRichQuality;
    const bool finalValid = valid && (pending == nullptr || pending->mask == m_allMask);
    if (!finalValid) {
        ++m_invalidPositions;
        m_blockHasInvalid = true;
    }

    const int kRaw = static_cast<int>(position / 2u);
    const int k = m_perChannelFrame > 0 ? kRaw % m_perChannelFrame : kRaw;
    const int kNext = m_perChannelFrame > 0 && kRaw >= m_perChannelFrame
        ? (kRaw + 1) % m_perChannelFrame : kRaw + 1;
    for (int s = 0; s < m_channelCount; ++s) {
        const int ch = m_physOfSel[s];
        const int base = slot * m_channelCount + s;
        const std::size_t rawOffset = static_cast<std::size_t>(base) *
                                       static_cast<std::size_t>(m_sampDepth);
        if (pending && (pending->mask & (1u << ch))) {
            std::memcpy(m_raw.data() + rawOffset, pending->lines[ch].data(),
                        static_cast<std::size_t>(m_sampDepth) * sizeof(float));
        }
        std::uint8_t wavelength = kUnknownWavelength;
        if (pending && pending->wavelengths[static_cast<std::size_t>(ch)] != kUnknownWavelength)
            wavelength = pending->wavelengths[static_cast<std::size_t>(ch)];
        else {
            const bool isWl1 = m_triggerWlOdd ? (position % 2u == 0u)
                                              : (position % 2u == 1u);
            wavelength = isWl1 ? 0 : 1;
        }
        const bool isWl1 = wavelength == 0;
        m_angles[base] = static_cast<float>(m_sectorStartDeg + s * m_sectorWidthDeg +
                                             (isWl1 ? k : kNext) * m_stepDeg);
        m_channels[base] = static_cast<std::uint8_t>(ch);
        m_wavelengths[base] = wavelength;
    }
    if (finalValid && slot < 64) m_validPositionBits |= std::uint64_t(1) << slot;
    m_blockTriggers.store(slot + 1, std::memory_order_relaxed);
    if (m_progressCallback) m_progressCallback();
    if (m_blockTriggers.load(std::memory_order_relaxed) == m_perChannelBlock)
        emitCurrentBlock();
}

void RingBlockAssembler::emitCurrentBlock()
{
    const int count = m_blockTriggers.load(std::memory_order_relaxed);
    if (count <= 0) return;
    const std::uint64_t validBits = m_validPositionBits;
    const bool rich = m_blockHasRichInput;
    if (m_callback) {
        if (m_ringCallback) {
            m_callback(std::vector<float>(m_raw), std::vector<float>(m_angles),
                       std::vector<uint8_t>(m_channels), m_blockSeq);
        } else {
            m_callback(std::move(m_raw), std::move(m_angles),
                       std::move(m_channels), m_blockSeq);
        }
    }
    if (m_ringCallback) {
        RingBlock block;
        block.serviceGeneration = m_serviceGeneration;
        block.roundId = m_roundId;
        block.configVersion = m_configVersion;
        block.blockSeq = static_cast<std::uint64_t>(m_blockSeq);
        block.startPosition = m_blockStartPosition;
        block.positionCount = static_cast<std::uint32_t>(count);
        block.validPositionBits = validBits;
        block.channelCount = static_cast<std::uint32_t>(m_channelCount);
        block.sampDepth = static_cast<std::uint32_t>(m_sampDepth);
        block.wavelengthAssumed = m_blockWavelengthAssumed || !rich;
        block.positionConfidence = m_positionConfidence;
        block.raw = std::move(m_raw);
        block.anglesDeg = std::move(m_angles);
        block.channels = std::move(m_channels);
        block.wavelengths = std::move(m_wavelengths);
        block.quality = m_blockQualityInitialized ? m_blockQuality : FrameQuality{};
        block.quality.qualityUnknown = !m_blockQualityInitialized || block.quality.qualityUnknown;
        block.quality.assemblyComplete = block.quality.assemblyComplete && !m_blockHasInvalid;
        block.quality.packetCoverageComplete = block.quality.packetCoverageComplete && !m_blockHasInvalid;
        block.quality.packetLengthValid = block.quality.packetLengthValid && !m_blockHasInvalid;
        block.quality.sampleLengthValid = block.quality.sampleLengthValid && !m_blockHasInvalid;
        if (m_blockHasInvalid)
            block.quality.missingReason = "position-gap-or-invalid-input";
        else if (!m_blockQualityInitialized)
            block.quality.missingReason = "quality-unavailable";
        m_ringCallback(std::move(block));
    }
    ++m_blockSeq;
    m_blockTriggers.store(0, std::memory_order_relaxed);
    m_blockStartPosition = m_nextPosition;
    m_validPositionBits = 0;
    m_blockHasInvalid = false;
    m_blockHasRichInput = false;
    m_blockQualityInitialized = false;
    m_blockQuality = FrameQuality{};
    m_blockWavelengthAssumed = false;
    m_raw = allocateRaw();
    const std::size_t lines = static_cast<std::size_t>(m_channelCount) *
                              static_cast<std::size_t>(m_perChannelBlock);
    m_angles.assign(lines, 0.0f);
    m_channels.assign(lines, 0);
    m_wavelengths.assign(lines, kUnknownWavelength);
}

void RingBlockAssembler::finishRound(RoundCloseReason reason)
{
    if (!m_configured) return;
    if (m_ringCallback) {
        publishReady(true);
        if (m_blockTriggers.load(std::memory_order_relaxed) > 0)
            emitCurrentBlock();
    }
    (void)reason;
    resetRoundState(true);
}

void RingBlockAssembler::resetRoundState(bool advanceRound)
{
    m_pending.clear();
    m_recent.clear();
    m_blockSeq = 0;
    m_blockTriggers.store(0, std::memory_order_relaxed);
    m_nextPendingOrder = 0;
    m_nextPosition = 0;
    m_highWaterPosition = 0;
    m_blockStartPosition = 0;
    m_haveWire = false;
    m_lastWire = 0;
    m_lastExpanded = 0;
    m_lastTriggerUs.store(0, std::memory_order_relaxed);
    m_validPositionBits = 0;
    m_blockHasInvalid = false;
    m_blockHasRichInput = false;
    m_blockQualityInitialized = false;
    m_blockQuality = FrameQuality{};
    m_blockWavelengthAssumed = false;
    m_raw = allocateRaw();
    const std::size_t lines = static_cast<std::size_t>(m_channelCount) *
                              static_cast<std::size_t>(m_perChannelBlock);
    m_angles.assign(lines, 0.0f);
    m_channels.assign(lines, 0);
    m_wavelengths.assign(lines, kUnknownWavelength);
    if (advanceRound) ++m_roundId;
}
