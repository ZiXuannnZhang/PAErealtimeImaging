#include "RoundTracker.h"

#include <algorithm>
#include <cmath>
#include <limits>

RoundTracker::RoundTracker() : RoundTracker(Config{}) {}

RoundTracker::RoundTracker(Config config) : config_(config) {
    configure(config);
}

void RoundTracker::configure(Config config)
{
    config.metadataCapacity = std::max<std::size_t>(1, config.metadataCapacity);
    config.reorderWindow = std::max<std::uint32_t>(1, config.reorderWindow);
    config_ = config;
    reset(RoundCloseReason::None);
}

void RoundTracker::reset(RoundCloseReason reason)
{
    if (current_.roundId == 0) current_.roundId = 1;
    current_.candidateStart = 0;
    current_.candidateEnd = 0;
    current_.basis = "first-visible-relative";
    current_.expectedCount = config_.expectedCount;
    current_.positionConfidence = PositionConfidence::RelativeOnly;
    current_.closeReason = reason;
    current_.unknownEpoch = reason == RoundCloseReason::SuspectedReset ||
                            reason == RoundCloseReason::HalfRangeAmbiguous;
    hasLast_ = false;
    lastWire_ = 0;
    lastExpanded_ = 0;
    relativeBase_ = 0;
    lastObservedNs_ = 0;
    recent_.clear();
}

void RoundTracker::closeRound(RoundCloseReason reason, bool unknown)
{
    beginRound(reason, unknown || reason == RoundCloseReason::SuspectedReset ||
               reason == RoundCloseReason::HalfRangeAmbiguous);
}

void RoundTracker::beginRound(RoundCloseReason reason, bool unknown)
{
    const auto next = current_.roundId == 0 ? 1 : current_.roundId + 1;
    current_ = RoundDescriptor{};
    current_.roundId = next;
    current_.expectedCount = config_.expectedCount;
    current_.basis = "first-visible-relative";
    current_.positionConfidence = unknown ? PositionConfidence::Unknown
                                          : PositionConfidence::RelativeOnly;
    current_.closeReason = reason;
    current_.unknownEpoch = unknown;
    hasLast_ = false;
    recent_.clear();
    relativeBase_ = 0;
}

void RoundTracker::remember(std::uint16_t wire, std::uint64_t expanded)
{
    recent_.push_back({wire, expanded});
    const std::size_t maxRecent = std::max<std::size_t>(64, config_.reorderWindow * 4u);
    while (recent_.size() > maxRecent) recent_.pop_front();
}

bool RoundTracker::findRemembered(std::uint16_t wire, std::uint64_t &expanded) const
{
    for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
        if (it->wire == wire) {
            expanded = it->expanded;
            return true;
        }
    }
    return false;
}

RoundTracker::Observation RoundTracker::observe(const FrameIdentity &identity,
                                                std::uint64_t observedMonotonicNs)
{
    Observation out;
    const bool metadataLoss = metadataDropped_.load(std::memory_order_relaxed) != 0;
    out.round = current_;
    out.round.expectedCount = config_.expectedCount;

    if (hasLast_ && config_.timeoutResetSec > 0.0 &&
        observedMonotonicNs > lastObservedNs_ &&
        static_cast<double>(observedMonotonicNs - lastObservedNs_) / 1e9 > config_.timeoutResetSec) {
        beginRound(RoundCloseReason::IdleTimeout, false);
        out.newRound = true;
    }
    if (metadataLoss) {
        current_.positionConfidence = PositionConfidence::Unknown;
        current_.basis = "metadata-drop";
        current_.unknownEpoch = true;
        out.metadataDropped = true;
    }

    if (!hasLast_) {
        hasLast_ = true;
        lastWire_ = identity.wireTrigger;
        lastExpanded_ = 0;
        relativeBase_ = 0;
        current_.candidateStart = identity.wireTrigger;
        current_.candidateEnd = identity.wireTrigger;
        remember(identity.wireTrigger, lastExpanded_);
        out.accepted = true;
        out.roundId = current_.roundId;
        out.expandedTrigger = lastExpanded_;
        out.relativePosition = 0;
        out.positionConfidence = current_.positionConfidence;
        out.round = current_;
        lastObservedNs_ = observedMonotonicNs;
        return out;
    }

    const std::int16_t delta = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(identity.wireTrigger - lastWire_));
    if (delta == 0) {
        out.duplicate = true;
        out.accepted = true;
        out.roundId = current_.roundId;
        out.expandedTrigger = lastExpanded_;
        out.relativePosition = lastExpanded_ - relativeBase_;
        out.positionConfidence = current_.positionConfidence;
        out.round = current_;
        lastObservedNs_ = std::max(lastObservedNs_, observedMonotonicNs);
        return out;
    }

    if (delta > 0) {
        lastExpanded_ += static_cast<std::uint16_t>(delta);
        lastWire_ = identity.wireTrigger;
        current_.candidateEnd = identity.wireTrigger;
        remember(identity.wireTrigger, lastExpanded_);
        out.accepted = true;
        out.roundId = current_.roundId;
        out.expandedTrigger = lastExpanded_;
        out.relativePosition = lastExpanded_ - relativeBase_;
        out.positionConfidence = current_.positionConfidence;
        out.round = current_;
        lastObservedNs_ = std::max(lastObservedNs_, observedMonotonicNs);
        return out;
    }

    std::uint64_t remembered = 0;
    if (findRemembered(identity.wireTrigger, remembered)) {
        out.accepted = true;
        out.late = true;
        out.roundId = current_.roundId;
        out.expandedTrigger = remembered;
        out.relativePosition = remembered - relativeBase_;
        out.positionConfidence = current_.positionConfidence;
        out.round = current_;
        return out;
    }

    const auto magnitude = static_cast<std::uint32_t>(-static_cast<std::int32_t>(delta));
    const bool ambiguous = delta == std::numeric_limits<std::int16_t>::min();
    const bool suspectedReset = ambiguous || magnitude > config_.reorderWindow;
    if (suspectedReset) {
        beginRound(ambiguous ? RoundCloseReason::HalfRangeAmbiguous
                             : RoundCloseReason::SuspectedReset,
                   true);
        out.newRound = true;
        out.unknownEpoch = true;
        hasLast_ = true;
        lastWire_ = identity.wireTrigger;
        lastExpanded_ = 0;
        relativeBase_ = 0;
        current_.candidateStart = identity.wireTrigger;
        current_.candidateEnd = identity.wireTrigger;
        remember(identity.wireTrigger, 0);
        out.accepted = true;
        out.roundId = current_.roundId;
        out.expandedTrigger = 0;
        out.relativePosition = 0;
        out.positionConfidence = PositionConfidence::Unknown;
        out.round = current_;
        lastObservedNs_ = observedMonotonicNs;
        return out;
    }

    // A small backward step is a reorder only when it remains inside the
    // already observed relative interval.  This preserves an internal hole
    // (0,2,1) without compressing it, while a packet before the first visible
    // anchor stays explicitly late/unknown instead of being assigned a fake
    // non-negative position.
    if (magnitude <= lastExpanded_ - relativeBase_) {
        const std::uint64_t candidate = lastExpanded_ - magnitude;
        remember(identity.wireTrigger, candidate);
        out.accepted = true;
        out.late = true;
        out.roundId = current_.roundId;
        out.expandedTrigger = candidate;
        out.relativePosition = candidate - relativeBase_;
        out.positionConfidence = current_.positionConfidence;
        out.round = current_;
        return out;
    }

    out.late = true;
    out.roundId = current_.roundId;
    out.positionConfidence = current_.positionConfidence;
    out.round = current_;
    return out;
}

bool RoundTracker::tryPublish(const Metadata &metadata) noexcept
{
    std::unique_lock<std::mutex> lock(metadataMutex_, std::try_to_lock);
    if (!lock.owns_lock() || metadataQueue_.size() >= config_.metadataCapacity) {
        metadataDropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    metadataQueue_.push_back(metadata);
    return true;
}

std::size_t RoundTracker::drain(std::vector<Metadata> &out, std::size_t maxItems)
{
    std::deque<Metadata> local;
    {
        std::lock_guard<std::mutex> lock(metadataMutex_);
        const auto count = std::min(maxItems, metadataQueue_.size());
        for (std::size_t i = 0; i < count; ++i) {
            local.push_back(std::move(metadataQueue_.front()));
            metadataQueue_.pop_front();
        }
    }
    const std::size_t drained = local.size();
    out.reserve(out.size() + drained);
    while (!local.empty()) {
        out.push_back(std::move(local.front()));
        local.pop_front();
    }
    return drained;
}

RoundTracker::Observation RoundTracker::observePublished(const Metadata &metadata)
{
    return observe(metadata.identity, metadata.observedMonotonicNs);
}

void RoundTracker::confirmWithOracle(std::uint64_t absoluteStart)
{
    if (!hasLast_) return;
    current_.candidateStart = absoluteStart;
    current_.positionConfidence = PositionConfidence::Confirmed;
    current_.basis = "external-oracle";
    current_.unknownEpoch = false;
}

RoundDescriptor RoundTracker::snapshot() const
{
    return current_;
}
