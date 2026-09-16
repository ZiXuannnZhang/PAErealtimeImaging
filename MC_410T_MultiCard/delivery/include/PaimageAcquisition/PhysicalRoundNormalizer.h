#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace paimage {

// The wire protocol exposes only the 16-bit trigger sequence.  The product
// contract therefore names the first new visible identity after a round
// boundary as the operational startup control trigger.  This is deliberately
// an operational classification, not a claim about a wire-level trigger type.
enum class PhysicalTriggerDecision : std::uint8_t {
    OperationalStartupControl = 0,
    LogicalScan = 1
};

enum class PhysicalRoundState : std::uint8_t {
    AwaitingControl = 0,
    CollectingScan = 1
};

struct PhysicalRoundClassification {
    PhysicalTriggerDecision decision = PhysicalTriggerDecision::LogicalScan;
    std::uint64_t measurementSession = 0;
    std::uint64_t roundGeneration = 0;
    std::uint16_t triggerSeq = 0;
    // -1 is used for the operational control trigger.
    std::int64_t logicalTriggerIndex = -1;
    bool newDistinct = false;
    // One-shot classification/control pulse: true only on the first
    // classification of the round's final logical trigger. Cached late cards
    // deliberately return false so control side effects stay one-shot.
    bool roundComplete = false;
    // Stable data-plane property: true for every classification — first or
    // cached — of the round's final logical trigger identity. The Ring data
    // plane consumes this so an enabled card still carries the terminal marker
    // when a disabled card was classified first.
    bool isFinalLogicalTrigger = false;
};

struct PhysicalRoundEvent {
    enum class Kind : std::uint8_t {
        ControlFiltered = 0,
        CountBoundary = 1,
        TimeoutBoundary = 2
    };

    Kind kind = Kind::ControlFiltered;
    std::uint64_t measurementSession = 0;
    std::uint64_t roundGeneration = 0;
    std::uint16_t triggerSeq = 0;
    std::uint64_t configuredLogicalTriggersPerRound = 0;
    std::uint64_t physicalDistinctObserved = 0;
    std::uint64_t operationalControlFiltered = 0;
    std::uint64_t logicalDistinctAccepted = 0;
    std::uint64_t countBoundaryResets = 0;
    std::uint64_t timeoutBoundaryResets = 0;
    std::uint64_t currentLogicalDistinctCount = 0;
    double timeoutResetSec = 0.0;
    std::int64_t idleDurationNs = 0;
    std::int64_t configuredTimeoutNs = 0;
    std::uint16_t lastDistinctTriggerSeq = 0;
    std::uint16_t nextVisibleTriggerSeq = 0;
    bool hasLastDistinctTriggerSeq = false;
    bool hasNextVisibleTriggerSeq = false;
    bool physicalRoundTimeoutEnabled = false;
    PhysicalRoundState state = PhysicalRoundState::AwaitingControl;
    // Stable machine-readable basis required by the operational contract.
    std::string basis = "first-visible-operational";
};

class PhysicalRoundNormalizer final {
public:
    struct Snapshot {
        std::uint64_t measurementSession = 0;
        std::uint64_t roundGeneration = 0;
        std::uint64_t configuredLogicalTriggersPerRound = 0;
        std::uint64_t physicalDistinctObserved = 0;
        std::uint64_t operationalControlFiltered = 0;
        std::uint64_t logicalDistinctAccepted = 0;
        std::uint64_t countBoundaryResets = 0;
        std::uint64_t timeoutBoundaryResets = 0;
        std::uint64_t currentLogicalDistinctCount = 0;
        double timeoutResetSec = 0.0;
        std::int64_t timeoutResetNs = 0;
        std::int64_t lastDistinctTriggerTimeNs = 0;
        std::uint16_t lastDistinctTriggerSeq = 0;
        bool hasLastDistinctTrigger = false;
        std::size_t recentDecisionCacheSize = 0;
        std::size_t recentDecisionCacheCapacity = 0;
        PhysicalRoundState state = PhysicalRoundState::AwaitingControl;
        bool firstVisibleFilterMode = true;
    };

    using Observer = std::function<void(const PhysicalRoundEvent&)>;

    // The cache capacity is a memory/late-card policy, not a round-size
    // default.  Production passes the configured logical round count
    // explicitly; zero is rejected so the normalizer cannot silently fall
    // back to a guessed 4000-trigger contract.
    explicit PhysicalRoundNormalizer(std::uint64_t configuredLogicalTriggersPerRound,
                                     Observer observer = {},
                                     std::size_t recentDecisionCacheCapacity = 8192);

    void beginSession(std::uint64_t measurementSession);

    // Classify one distinct physical identity. Repeated cards for the same
    // session/trigger return the cached decision and never advance logical
    // counters; the cached generation is retained for diagnostics.
    PhysicalRoundClassification classify(std::uint64_t measurementSession,
                                         std::uint16_t triggerSeq,
                                         std::int64_t observedMonotonicNs = 0);

    // Clear a partial logical round once, leaving an already-idle boundary
    // untouched. This makes count-boundary followed by timeout idempotent.
    void timeoutBoundary(std::uint64_t measurementSession,
                         std::int64_t observedMonotonicNs = 0);

    // The product source is RingReconCudaConfig.timeoutResetSec. Updating it
    // is configuration only: it never creates a boundary or filters a
    // trigger by itself.
    void setTimeoutResetSec(double seconds);
    double timeoutResetSec() const;

    // Used when the canonical round count is changed before the next
    // measurement. A live session is made to await a new control identity.
    void setConfiguredLogicalTriggersPerRound(std::uint64_t count);

    Snapshot snapshot() const;
    std::uint64_t configuredLogicalTriggersPerRound() const;

private:
    struct CachedDecision {
        std::uint64_t measurementSession = 0;
        std::uint64_t roundGeneration = 0;
        std::uint16_t triggerSeq = 0;
        PhysicalRoundClassification classification;
    };

    PhysicalRoundEvent eventLocked(PhysicalRoundEvent::Kind kind,
                                   std::uint16_t triggerSeq,
                                   const char* basis = "first-visible-operational",
                                   std::int64_t idleDurationNs = 0,
                                   bool hasLastDistinctTriggerSeq = false,
                                   std::uint16_t lastDistinctTriggerSeq = 0,
                                   bool hasNextVisibleTriggerSeq = false,
                                   std::uint16_t nextVisibleTriggerSeq = 0) const;
    void resetSessionLocked(std::uint64_t measurementSession);
    static std::int64_t timeoutToNs(double seconds);
    void notify(const PhysicalRoundEvent& event) const;
    void trimCacheLocked();

    mutable std::mutex mutex_;
    std::uint64_t measurementSession_ = 0;
    std::uint64_t roundGeneration_ = 0;
    std::uint64_t configuredLogicalTriggersPerRound_ = 0;
    std::uint64_t physicalDistinctObserved_ = 0;
    std::uint64_t operationalControlFiltered_ = 0;
    std::uint64_t logicalDistinctAccepted_ = 0;
    std::uint64_t countBoundaryResets_ = 0;
    std::uint64_t timeoutBoundaryResets_ = 0;
    std::uint64_t currentLogicalDistinctCount_ = 0;
    double timeoutResetSec_ = 0.0;
    std::int64_t timeoutResetNs_ = 0;
    std::int64_t lastDistinctTriggerTimeNs_ = 0;
    std::uint16_t lastDistinctTriggerSeq_ = 0;
    bool hasLastDistinctTrigger_ = false;
    PhysicalRoundState state_ = PhysicalRoundState::AwaitingControl;
    std::size_t cacheCapacity_ = 0;
    std::deque<CachedDecision> recentDecisions_;
    Observer observer_;
};

const char* physicalTriggerDecisionName(PhysicalTriggerDecision decision);
const char* physicalRoundStateName(PhysicalRoundState state);
const char* physicalRoundEventName(PhysicalRoundEvent::Kind kind);

} // namespace paimage
