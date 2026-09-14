#include "PaimageAcquisition/PhysicalRoundNormalizer.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace paimage {

namespace {
constexpr const char* kBasis = "first-visible-operational";
}

PhysicalRoundNormalizer::PhysicalRoundNormalizer(
    std::uint64_t configuredLogicalTriggersPerRound,
    Observer observer,
    std::size_t recentDecisionCacheCapacity)
    : configuredLogicalTriggersPerRound_(configuredLogicalTriggersPerRound),
      cacheCapacity_(recentDecisionCacheCapacity),
      observer_(std::move(observer)) {
    if (configuredLogicalTriggersPerRound_ == 0)
        throw std::invalid_argument("logical round count must be positive");
    if (cacheCapacity_ == 0)
        throw std::invalid_argument("recent decision cache capacity must be positive");
}

void PhysicalRoundNormalizer::beginSession(std::uint64_t measurementSession) {
    std::lock_guard<std::mutex> lock(mutex_);
    measurementSession_ = measurementSession;
    roundGeneration_ = 0;
    physicalDistinctObserved_ = 0;
    operationalControlFiltered_ = 0;
    logicalDistinctAccepted_ = 0;
    countBoundaryResets_ = 0;
    timeoutBoundaryResets_ = 0;
    currentLogicalDistinctCount_ = 0;
    state_ = PhysicalRoundState::AwaitingControl;
    recentDecisions_.clear();
}

PhysicalRoundEvent PhysicalRoundNormalizer::eventLocked(
    PhysicalRoundEvent::Kind kind, std::uint16_t triggerSeq) const {
    PhysicalRoundEvent event;
    event.kind = kind;
    event.measurementSession = measurementSession_;
    event.roundGeneration = roundGeneration_;
    event.triggerSeq = triggerSeq;
    event.configuredLogicalTriggersPerRound = configuredLogicalTriggersPerRound_;
    event.physicalDistinctObserved = physicalDistinctObserved_;
    event.operationalControlFiltered = operationalControlFiltered_;
    event.logicalDistinctAccepted = logicalDistinctAccepted_;
    event.countBoundaryResets = countBoundaryResets_;
    event.timeoutBoundaryResets = timeoutBoundaryResets_;
    event.currentLogicalDistinctCount = currentLogicalDistinctCount_;
    event.state = state_;
    event.basis = kBasis;
    return event;
}

void PhysicalRoundNormalizer::notify(const PhysicalRoundEvent& event) const {
    if (observer_)
        observer_(event);
}

void PhysicalRoundNormalizer::trimCacheLocked() {
    while (recentDecisions_.size() > cacheCapacity_)
        recentDecisions_.pop_front();
}

PhysicalRoundClassification PhysicalRoundNormalizer::classify(
    std::uint64_t measurementSession, std::uint16_t triggerSeq) {
    PhysicalRoundEvent event;
    bool hasEvent = false;
    PhysicalRoundClassification result;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // A caller that missed the explicit lifecycle hook still gets a
        // clean session boundary. Production calls beginSession before the
        // receiver is armed, so this path is mainly a defensive test seam.
        if (measurementSession_ != measurementSession) {
            measurementSession_ = measurementSession;
            roundGeneration_ = 0;
            physicalDistinctObserved_ = 0;
            operationalControlFiltered_ = 0;
            logicalDistinctAccepted_ = 0;
            countBoundaryResets_ = 0;
            timeoutBoundaryResets_ = 0;
            currentLogicalDistinctCount_ = 0;
            state_ = PhysicalRoundState::AwaitingControl;
            recentDecisions_.clear();
        }

        // Search newest first. The wire identity is limited to the measurement
        // session and uint16 trigger sequence, so the bounded cache retains
        // prior decisions for late cards after a count/timeout boundary. The
        // stored generation remains available for diagnostics; identities
        // outside the bounded cache are treated as new observations.
        for (auto it = recentDecisions_.rbegin(); it != recentDecisions_.rend(); ++it) {
            if (it->measurementSession == measurementSession_ &&
                it->triggerSeq == triggerSeq) {
                auto cached = it->classification;
                cached.newDistinct = false;
                // roundComplete is a one-shot boundary signal. Late cards
                // must reuse the logical decision/index without repeating the
                // Ring/reconstruction reset for the same physical identity.
                cached.roundComplete = false;
                return cached;
            }
        }

        result.measurementSession = measurementSession_;
        result.roundGeneration = roundGeneration_;
        result.triggerSeq = triggerSeq;
        result.newDistinct = true;
        ++physicalDistinctObserved_;

        if (state_ == PhysicalRoundState::AwaitingControl) {
            result.decision = PhysicalTriggerDecision::OperationalStartupControl;
            result.logicalTriggerIndex = -1;
            state_ = PhysicalRoundState::CollectingScan;
            ++operationalControlFiltered_;
            event = eventLocked(PhysicalRoundEvent::Kind::ControlFiltered, triggerSeq);
            hasEvent = true;
        } else {
            result.decision = PhysicalTriggerDecision::LogicalScan;
            result.logicalTriggerIndex = static_cast<std::int64_t>(currentLogicalDistinctCount_);
            ++currentLogicalDistinctCount_;
            ++logicalDistinctAccepted_;
            if (currentLogicalDistinctCount_ == configuredLogicalTriggersPerRound_) {
                result.roundComplete = true;
                ++countBoundaryResets_;
                currentLogicalDistinctCount_ = 0;
                state_ = PhysicalRoundState::AwaitingControl;
                ++roundGeneration_;
                event = eventLocked(PhysicalRoundEvent::Kind::CountBoundary, triggerSeq);
                hasEvent = true;
            }
        }

        recentDecisions_.push_back({measurementSession_, result.roundGeneration,
                                     triggerSeq, result});
        trimCacheLocked();
    }
    if (hasEvent)
        notify(event);
    return result;
}

void PhysicalRoundNormalizer::timeoutBoundary(std::uint64_t measurementSession) {
    PhysicalRoundEvent event;
    bool hasEvent = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (measurementSession_ != measurementSession) {
            measurementSession_ = measurementSession;
            roundGeneration_ = 0;
            physicalDistinctObserved_ = 0;
            operationalControlFiltered_ = 0;
            logicalDistinctAccepted_ = 0;
            countBoundaryResets_ = 0;
            timeoutBoundaryResets_ = 0;
            currentLogicalDistinctCount_ = 0;
            state_ = PhysicalRoundState::AwaitingControl;
            recentDecisions_.clear();
        }
        if (state_ != PhysicalRoundState::CollectingScan)
            return;
        currentLogicalDistinctCount_ = 0;
        state_ = PhysicalRoundState::AwaitingControl;
        ++roundGeneration_;
        ++timeoutBoundaryResets_;
        event = eventLocked(PhysicalRoundEvent::Kind::TimeoutBoundary, 0);
        hasEvent = true;
    }
    if (hasEvent)
        notify(event);
}

void PhysicalRoundNormalizer::setConfiguredLogicalTriggersPerRound(std::uint64_t count) {
    if (count == 0)
        throw std::invalid_argument("logical round count must be positive");
    std::lock_guard<std::mutex> lock(mutex_);
    if (configuredLogicalTriggersPerRound_ == count)
        return;
    configuredLogicalTriggersPerRound_ = count;
    currentLogicalDistinctCount_ = 0;
    state_ = PhysicalRoundState::AwaitingControl;
    ++roundGeneration_;
    recentDecisions_.clear();
}

PhysicalRoundNormalizer::Snapshot PhysicalRoundNormalizer::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot result;
    result.measurementSession = measurementSession_;
    result.roundGeneration = roundGeneration_;
    result.configuredLogicalTriggersPerRound = configuredLogicalTriggersPerRound_;
    result.physicalDistinctObserved = physicalDistinctObserved_;
    result.operationalControlFiltered = operationalControlFiltered_;
    result.logicalDistinctAccepted = logicalDistinctAccepted_;
    result.countBoundaryResets = countBoundaryResets_;
    result.timeoutBoundaryResets = timeoutBoundaryResets_;
    result.currentLogicalDistinctCount = currentLogicalDistinctCount_;
    result.recentDecisionCacheSize = recentDecisions_.size();
    result.recentDecisionCacheCapacity = cacheCapacity_;
    result.state = state_;
    return result;
}

std::uint64_t PhysicalRoundNormalizer::configuredLogicalTriggersPerRound() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return configuredLogicalTriggersPerRound_;
}

const char* physicalTriggerDecisionName(PhysicalTriggerDecision decision) {
    switch (decision) {
    case PhysicalTriggerDecision::OperationalStartupControl:
        return "OperationalStartupControl";
    case PhysicalTriggerDecision::LogicalScan:
        return "LogicalScan";
    }
    return "Unknown";
}

const char* physicalRoundStateName(PhysicalRoundState state) {
    switch (state) {
    case PhysicalRoundState::AwaitingControl:
        return "AwaitingControl";
    case PhysicalRoundState::CollectingScan:
        return "CollectingScan";
    }
    return "Unknown";
}

const char* physicalRoundEventName(PhysicalRoundEvent::Kind kind) {
    switch (kind) {
    case PhysicalRoundEvent::Kind::ControlFiltered:
        return "round_control_filtered";
    case PhysicalRoundEvent::Kind::CountBoundary:
        return "count_boundary_reset";
    case PhysicalRoundEvent::Kind::TimeoutBoundary:
        return "timeout_boundary_reset";
    }
    return "unknown";
}

} // namespace paimage
