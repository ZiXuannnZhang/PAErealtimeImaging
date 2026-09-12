#include "RingPipeline.h"

#include <algorithm>
#include <chrono>

void RingPipeline::configure(const int enabledChannels[8], int perChannelBlock,
                             int sampDepth, double sectorStartDeg,
                             double sectorWidthDeg, double stepDeg,
                             int perChannelFrame, int triggerWlOdd,
                             double timeoutResetSec)
{
    RingBlockAssembler::configure(enabledChannels, perChannelBlock, sampDepth,
                                  sectorStartDeg, sectorWidthDeg, stepDeg,
                                  perChannelFrame, triggerWlOdd, timeoutResetSec);
    RoundTracker::Config config;
    config.timeoutResetSec = timeoutResetSec;
    config.configVersion = configVersion_;
    tracker_.configure(config);
    observedWires_.clear();
    RingBlockAssembler::setIdentityContext(serviceGeneration_, 1,
                                            configVersion_,
                                            PositionConfidence::RelativeOnly);
}

void RingPipeline::reset()
{
    RingBlockAssembler::reset();
    tracker_.reset();
    observedWires_.clear();
    RingBlockAssembler::setIdentityContext(serviceGeneration_, 1,
                                            configVersion_,
                                            PositionConfidence::RelativeOnly);
}

void RingPipeline::setIdentityContext(std::uint64_t serviceGeneration,
                                      std::uint64_t roundId,
                                      std::uint64_t configVersion,
                                      PositionConfidence confidence)
{
    serviceGeneration_ = serviceGeneration;
    configVersion_ = configVersion;
    RingBlockAssembler::setIdentityContext(serviceGeneration, roundId,
                                            configVersion, confidence);
    auto config = RoundTracker::Config{};
    config.measurementSession = 0;
    config.configVersion = configVersion;
    config.timeoutResetSec = timeoutResetSec();
    tracker_.configure(config);
    observedWires_.clear();
}

void RingPipeline::observeTrigger(const FrameIdentity &identity)
{
    if (std::find(observedWires_.begin(), observedWires_.end(),
                  identity.wireTrigger) != observedWires_.end()) return;
    const auto now = identity.firstReceiveMonotonicNs != 0
        ? identity.firstReceiveMonotonicNs
        : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!tracker_.tryPublish({identity, now})) return;
    std::vector<RoundTracker::Metadata> published;
    tracker_.drain(published);
    for (const auto &metadata : published) {
        if (std::find(observedWires_.begin(), observedWires_.end(),
                      metadata.identity.wireTrigger) != observedWires_.end())
            continue;
        observedWires_.push_back(metadata.identity.wireTrigger);
        while (observedWires_.size() > 256) observedWires_.pop_front();
        const auto observation = tracker_.observePublished(metadata);
        if (observation.newRound) {
            RingBlockAssembler::finishRound(observation.round.closeReason);
        }
        RingBlockAssembler::setIdentityContext(serviceGeneration_, observation.roundId,
                                                metadata.identity.configVersion != 0
                                                    ? metadata.identity.configVersion : configVersion_,
                                                observation.positionConfidence);
    }
}

void RingPipeline::pushChannelLine(int channelId, std::uint16_t triggerSeq,
                                   const float *line, int length,
                                   const FrameIdentity &identity,
                                   const FrameQuality &quality,
                                   int wavelength)
{
    FrameIdentity normalized = identity;
    normalized.wireTrigger = triggerSeq;
    observeTrigger(normalized);
    RingBlockAssembler::pushChannelLine(channelId, triggerSeq, line, length,
                                        identity, quality, wavelength);
}

void RingPipeline::finishRound(RoundCloseReason reason)
{
    RingBlockAssembler::finishRound(reason);
    tracker_.closeRound(reason);
    observedWires_.clear();
    const auto snapshot = tracker_.snapshot();
    RingBlockAssembler::setIdentityContext(serviceGeneration_, snapshot.roundId,
                                            configVersion_, snapshot.positionConfidence);
}

void RingPipeline::setTimeoutCallback(TimeoutCallback callback)
{
    RingBlockAssembler::setTimeoutCallback([this, callback = std::move(callback)] {
        tracker_.closeRound(RoundCloseReason::IdleTimeout);
        observedWires_.clear();
        const auto snapshot = tracker_.snapshot();
        RingBlockAssembler::setIdentityContext(serviceGeneration_, snapshot.roundId,
                                                configVersion_, snapshot.positionConfidence);
        if (callback) callback();
    });
}
