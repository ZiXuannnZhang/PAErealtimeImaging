#pragma once

#include "RingTypes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <utility>
#include <vector>

// Position-owned ring block assembly. The legacy callback is retained for the
// existing UI/self-test surface; RingBlockCallback is the new contract.
// Both callbacks receive the same fixed-width raw layout. Missing positions
// remain zero-filled and are excluded by validPositionBits.
class RingBlockAssembler
{
public:
    using BlockCallback = std::function<void(std::vector<float> &&raw,
                                             std::vector<float> &&anglesDeg,
                                             std::vector<uint8_t> &&channels,
                                             int blockSeq)>;
    using RingBlockCallback = std::function<void(RingBlock &&)>;
    using ProgressCallback = std::function<void()>;
    using TimeoutCallback = std::function<void()>;

    void configure(const int enabledChannels[8], int perChannelBlock,
                   int sampDepth, double sectorStartDeg, double sectorWidthDeg,
                   double stepDeg, int perChannelFrame, int triggerWlOdd,
                   double timeoutResetSec);

    void reset();

    // Compatibility entry point. It preserves old zero-padding behavior for
    // the legacy callback; rich blocks mark a short line invalid.
    void pushChannelLine(int channelId, uint16_t triggerSeq,
                         const float *line, int length);

    // Rich entry point carrying immutable frame identity/quality and explicit
    // wavelength. wavelength is 0=WL1, 1=WL2; -1 uses the configured relative
    // fallback and is marked assumed in the emitted block.
    void pushChannelLine(int channelId, uint16_t triggerSeq,
                         const float *line, int length,
                         const FrameIdentity &identity,
                         const FrameQuality &quality,
                         int wavelength = -1);

    // Publish the current position interval and a 1..49 tail without mixing
    // it with the next round. The next input starts at relative position 0.
    void finishRound(RoundCloseReason reason = RoundCloseReason::ExplicitStop);

    void setBlockCallback(BlockCallback cb) { m_callback = std::move(cb); }
    void setRingBlockCallback(RingBlockCallback cb) { m_ringCallback = std::move(cb); }
    void setProgressCallback(ProgressCallback cb) { m_progressCallback = std::move(cb); }
    void setTimeoutCallback(TimeoutCallback cb) { m_timeoutCallback = std::move(cb); }
    void setIdentityContext(std::uint64_t serviceGeneration,
                            std::uint64_t roundId,
                            std::uint64_t configVersion,
                            PositionConfidence confidence = PositionConfidence::RelativeOnly);

    std::pair<int, int> blockProgress() const {
        return { m_blockTriggers.load(std::memory_order_relaxed), m_perChannelBlock };
    }
    double timeoutResetSec() const { return m_timeoutResetSec; }
    double idleSeconds() const {
        const int64_t us = m_lastTriggerUs.load(std::memory_order_relaxed);
        if (us <= 0) return 0.0;
        const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return static_cast<double>(nowUs - us) / 1e6;
    }

    std::uint64_t lateAfterSeal() const noexcept { return m_lateAfterSeal; }
    std::uint64_t invalidPositions() const noexcept { return m_invalidPositions; }
    std::uint64_t currentPosition() const noexcept { return m_nextPosition; }

private:
    struct PendingTrigger {
        std::array<std::vector<float>, 8> lines;
        std::array<std::uint8_t, 8> wavelengths{};
        std::uint32_t mask = 0;
        std::uint64_t firstSeenOrder = 0;
        std::uint64_t position = 0;
        FrameIdentity identity;
        FrameQuality quality;
        bool hasRichQuality = false;
        bool invalidLine = false;
    };

    struct RecentTrigger {
        std::uint16_t wire = 0;
        std::uint64_t position = 0;
    };

    void pushChannelLineImpl(int channelId, uint16_t triggerSeq,
                             const float *line, int length,
                             const FrameIdentity *identity,
                             const FrameQuality *quality,
                             int wavelength);
    bool expandTrigger(uint16_t triggerSeq, std::uint64_t &position);
    void rememberTrigger(uint16_t wire, std::uint64_t position);
    bool findRecent(uint16_t wire, std::uint64_t &position) const;
    bool acceptNewRound(RoundCloseReason reason, bool notify);
    void publishReady(bool force);
    void appendPosition(const PendingTrigger *pending,
                        std::uint64_t position,
                        bool valid);
    void mergeBlockQuality(const FrameQuality &quality);
    void emitCurrentBlock();
    void resetRoundState(bool advanceRound);
    std::vector<float> allocateRaw() const;

    BlockCallback m_callback;
    RingBlockCallback m_ringCallback;
    ProgressCallback m_progressCallback;
    TimeoutCallback m_timeoutCallback;
    bool m_configured = false;
    int m_enabled[8] = {0};
    int m_physOfSel[8] = {-1};
    int m_channelCount = 0;
    int m_perChannelBlock = 0;
    int m_sampDepth = 0;
    double m_sectorStartDeg = 0.0;
    double m_sectorWidthDeg = 0.0;
    double m_stepDeg = 0.0;
    int m_perChannelFrame = 0;
    int m_triggerWlOdd = 1;
    double m_timeoutResetSec = 0.0;
    std::atomic<int64_t> m_lastTriggerUs{0};

    std::uint64_t m_serviceGeneration = 0;
    std::uint64_t m_roundId = 1;
    std::uint64_t m_configVersion = 0;
    PositionConfidence m_positionConfidence = PositionConfidence::RelativeOnly;

    int m_blockSeq = 0;
    std::atomic<int> m_blockTriggers{0};
    std::uint64_t m_nextPendingOrder = 0;
    std::uint64_t m_nextPosition = 0;
    std::uint64_t m_highWaterPosition = 0;
    std::uint64_t m_blockStartPosition = 0;
    bool m_haveWire = false;
    std::uint16_t m_lastWire = 0;
    std::uint64_t m_lastExpanded = 0;
    std::uint32_t m_allMask = 0;
    std::map<std::uint64_t, PendingTrigger> m_pending;
    std::deque<RecentTrigger> m_recent;

    std::vector<float> m_raw;
    std::vector<float> m_angles;
    std::vector<uint8_t> m_channels;
    std::vector<uint8_t> m_wavelengths;
    std::uint64_t m_validPositionBits = 0;
    bool m_blockHasInvalid = false;
    bool m_blockHasRichInput = false;
    bool m_blockQualityInitialized = false;
    FrameQuality m_blockQuality;
    bool m_blockWavelengthAssumed = true;
    std::uint64_t m_lateAfterSeal = 0;
    std::uint64_t m_invalidPositions = 0;
};
