#include "PaimageAcquisition/PhysicalRoundNormalizer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace paimage {

namespace {
constexpr const char* kBasis = "first-visible-operational";
constexpr const char* kTimeoutBasis = "physical-idle-timeout";
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

std::int64_t PhysicalRoundNormalizer::timeoutToNs(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0)
        throw std::invalid_argument("physical round timeout must be finite and non-negative");
    if (seconds == 0.0)
        return 0;
    const long double ns = static_cast<long double>(seconds) * 1000000000.0L;
    if (ns > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        throw std::out_of_range("physical round timeout is too large");
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(ns)));
}

void PhysicalRoundNormalizer::resetSessionLocked(std::uint64_t measurementSession) {
    measurementSession_ = measurementSession;
    roundGeneration_ = 0;
    physicalDistinctObserved_ = 0;
    operationalControlFiltered_ = 0;
    logicalDistinctAccepted_ = 0;
    countBoundaryResets_ = 0;
    timeoutBoundaryResets_ = 0;
    currentLogicalDistinctCount_ = 0;
    lastDistinctTriggerTimeNs_ = 0;
    lastDistinctTriggerSeq_ = 0;
    hasLastDistinctTrigger_ = false;
    state_ = PhysicalRoundState::AwaitingControl;
    recentDecisions_.clear();
}

void PhysicalRoundNormalizer::beginSession(std::uint64_t measurementSession) {
    std::lock_guard<std::mutex> lock(mutex_);
    resetSessionLocked(measurementSession);
}

PhysicalRoundEvent PhysicalRoundNormalizer::eventLocked(
    PhysicalRoundEvent::Kind kind, std::uint16_t triggerSeq, const char* basis,
    std::int64_t idleDurationNs, bool hasLastDistinctTriggerSeq,
    std::uint16_t lastDistinctTriggerSeq, bool hasNextVisibleTriggerSeq,
    std::uint16_t nextVisibleTriggerSeq) const {
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
    event.timeoutResetSec = timeoutResetSec_;
    event.idleDurationNs = idleDurationNs;
    event.configuredTimeoutNs = timeoutResetNs_;
    event.lastDistinctTriggerSeq = lastDistinctTriggerSeq;
    event.nextVisibleTriggerSeq = nextVisibleTriggerSeq;
    event.hasLastDistinctTriggerSeq = hasLastDistinctTriggerSeq;
    event.hasNextVisibleTriggerSeq = hasNextVisibleTriggerSeq;
    event.physicalRoundTimeoutEnabled = timeoutResetNs_ > 0;
    event.state = state_;
    event.basis = basis ? basis : kBasis;
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
    std::uint64_t measurementSession, std::uint16_t triggerSeq,
    std::int64_t observedMonotonicNs) {
    std::vector<PhysicalRoundEvent> events;
    PhysicalRoundClassification result;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // A caller that missed the explicit lifecycle hook still gets a
        // clean session boundary. Production calls beginSession before the
        // receiver is armed, so this path is mainly a defensive test seam.
        if (measurementSession_ != measurementSession)
            resetSessionLocked(measurementSession);

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
                // isFinalLogicalTrigger is a stable per-trigger data property:
                // it stays true on cached hits so the Ring final marker
                // survives a disabled card being classified first.
                cached.roundComplete = false;
                return cached;
            }
        }

        // Only a new physical identity can close an idle gap. A late card
        // hits the cache above and must not refresh this anchor or create a
        // timeout boundary.
        if (state_ == PhysicalRoundState::CollectingScan &&
            timeoutResetNs_ > 0 && hasLastDistinctTrigger_ &&
            observedMonotonicNs >= lastDistinctTriggerTimeNs_ &&
            observedMonotonicNs - lastDistinctTriggerTimeNs_ >= timeoutResetNs_) {
            const auto idleDurationNs = observedMonotonicNs - lastDistinctTriggerTimeNs_;
            currentLogicalDistinctCount_ = 0;
            state_ = PhysicalRoundState::AwaitingControl;
            ++roundGeneration_;
            ++timeoutBoundaryResets_;
            events.push_back(eventLocked(
                PhysicalRoundEvent::Kind::TimeoutBoundary, triggerSeq,
                kTimeoutBasis, idleDurationNs, hasLastDistinctTrigger_,
                lastDistinctTriggerSeq_, true, triggerSeq));
        }

        result.measurementSession = measurementSession_;
        result.roundGeneration = roundGeneration_;
        result.triggerSeq = triggerSeq;
        result.newDistinct = true;
        ++physicalDistinctObserved_;

        // Frame timestamps are monotonic ingress timestamps. Do not move the
        // anchor backwards if a malformed/out-of-order source timestamp is
        // presented; importantly, repeated cards never reach this block.
        if (observedMonotonicNs > 0 &&
            (!hasLastDistinctTrigger_ ||
             observedMonotonicNs >= lastDistinctTriggerTimeNs_)) {
            lastDistinctTriggerTimeNs_ = observedMonotonicNs;
            lastDistinctTriggerSeq_ = triggerSeq;
            hasLastDistinctTrigger_ = true;
        }

        if (state_ == PhysicalRoundState::AwaitingControl) {
            result.decision = PhysicalTriggerDecision::OperationalStartupControl;
            result.logicalTriggerIndex = -1;
            state_ = PhysicalRoundState::CollectingScan;
            ++operationalControlFiltered_;
            events.push_back(eventLocked(PhysicalRoundEvent::Kind::ControlFiltered, triggerSeq));
        } else {
            result.decision = PhysicalTriggerDecision::LogicalScan;
            result.logicalTriggerIndex = static_cast<std::int64_t>(currentLogicalDistinctCount_);
            ++currentLogicalDistinctCount_;
            ++logicalDistinctAccepted_;
            if (currentLogicalDistinctCount_ == configuredLogicalTriggersPerRound_) {
                result.roundComplete = true;
                result.isFinalLogicalTrigger = true;
                ++countBoundaryResets_;
                currentLogicalDistinctCount_ = 0;
                state_ = PhysicalRoundState::AwaitingControl;
                ++roundGeneration_;
                events.push_back(eventLocked(PhysicalRoundEvent::Kind::CountBoundary, triggerSeq));
            }
        }

        recentDecisions_.push_back({measurementSession_, result.roundGeneration,
                                     triggerSeq, result});
        trimCacheLocked();
    }
    for (const auto& event : events)
        notify(event);
    return result;
}

void PhysicalRoundNormalizer::timeoutBoundary(std::uint64_t measurementSession,
                                              std::int64_t observedMonotonicNs) {
    PhysicalRoundEvent event;
    bool hasEvent = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (measurementSession_ != measurementSession)
            resetSessionLocked(measurementSession);
        if (state_ != PhysicalRoundState::CollectingScan)
            return;
        currentLogicalDistinctCount_ = 0;
        state_ = PhysicalRoundState::AwaitingControl;
        ++roundGeneration_;
        ++timeoutBoundaryResets_;
        if (observedMonotonicNs > 0 &&
            (!hasLastDistinctTrigger_ ||
             observedMonotonicNs >= lastDistinctTriggerTimeNs_)) {
            lastDistinctTriggerTimeNs_ = observedMonotonicNs;
        }
        event = eventLocked(PhysicalRoundEvent::Kind::TimeoutBoundary, 0,
                            kTimeoutBasis, 0, hasLastDistinctTrigger_,
                            lastDistinctTriggerSeq_);
        hasEvent = true;
    }
    if (hasEvent)
        notify(event);
}

void PhysicalRoundNormalizer::setTimeoutResetSec(double seconds) {
    const auto timeoutNs = timeoutToNs(seconds);
    std::lock_guard<std::mutex> lock(mutex_);
    timeoutResetSec_ = seconds;
    timeoutResetNs_ = timeoutNs;
}

double PhysicalRoundNormalizer::timeoutResetSec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timeoutResetSec_;
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
    result.timeoutResetSec = timeoutResetSec_;
    result.timeoutResetNs = timeoutResetNs_;
    result.lastDistinctTriggerTimeNs = lastDistinctTriggerTimeNs_;
    result.lastDistinctTriggerSeq = lastDistinctTriggerSeq_;
    result.hasLastDistinctTrigger = hasLastDistinctTrigger_;
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
