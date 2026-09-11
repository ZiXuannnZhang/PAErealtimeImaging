#pragma once

#include "RingBlockAssembler.h"
#include "RoundTracker.h"

#include <deque>

// The sole owner of the ring downstream state.  It keeps trigger identity
// tracking and position-owned block assembly behind one worker boundary; the
// receiver/saver never calls the assembler directly.
class RingPipeline final : public RingBlockAssembler {
public:
    using RingBlockAssembler::pushChannelLine;
    using RingBlockAssembler::setBlockCallback;
    using RingBlockAssembler::setProgressCallback;

    void configure(const int enabledChannels[8], int perChannelBlock,
                   int sampDepth, double sectorStartDeg, double sectorWidthDeg,
                   double stepDeg, int perChannelFrame, int triggerWlOdd,
                   double timeoutResetSec);
    void reset();
    void finishRound(RoundCloseReason reason = RoundCloseReason::ExplicitStop);
    void setIdentityContext(std::uint64_t serviceGeneration,
                            std::uint64_t roundId,
                            std::uint64_t configVersion,
                            PositionConfidence confidence = PositionConfidence::RelativeOnly);
    void setTimeoutCallback(TimeoutCallback callback);

    void pushChannelLine(int channelId, std::uint16_t triggerSeq,
                         const float *line, int length,
                         const FrameIdentity &identity,
                         const FrameQuality &quality,
                         int wavelength = -1);

    RoundDescriptor roundSnapshot() const { return tracker_.snapshot(); }
    std::uint64_t metadataDropped() const noexcept { return tracker_.metadataDropped(); }

private:
    void observeTrigger(const FrameIdentity &identity);

    RoundTracker tracker_;
    std::uint64_t serviceGeneration_ = 0;
    std::uint64_t configVersion_ = 0;
    std::deque<std::uint16_t> observedWires_;
};
