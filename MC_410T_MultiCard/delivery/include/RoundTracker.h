#pragma once

#include "RingTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// The receive path publishes scalar metadata without waiting.  RoundTracker
// owns all epoch/position state on one consumer thread and exposes immutable
// observations to downstream users.
class RoundTracker final {
public:
    struct Config {
        std::uint64_t measurementSession = 0;
        std::uint64_t configVersion = 0;
        std::uint32_t expectedCount = 4001;
        double timeoutResetSec = 0.0;
        std::size_t metadataCapacity = 8192;
        std::uint32_t reorderWindow = 32;
    };

    struct Observation {
        bool accepted = false;
        bool duplicate = false;
        bool late = false;
        bool newRound = false;
        bool unknownEpoch = false;
        bool metadataDropped = false;
        std::uint64_t roundId = 0;
        std::uint64_t expandedTrigger = 0;
        std::uint64_t relativePosition = 0;
        PositionConfidence positionConfidence = PositionConfidence::RelativeOnly;
        RoundDescriptor round;
    };

    struct Metadata {
        FrameIdentity identity;
        std::uint64_t observedMonotonicNs = 0;
    };

    RoundTracker();
    explicit RoundTracker(Config config);

    void configure(Config config);
    void reset(RoundCloseReason reason = RoundCloseReason::None);
    // Close the current relative epoch and advance to a fresh candidate.
    // This is used by the owning RingPipeline at explicit lifecycle boundaries;
    // reset() remains the full state reset used during initial configuration.
    void closeRound(RoundCloseReason reason, bool unknown = false);

    // Owner-thread operation.  It never performs I/O and only updates scalar
    // state plus a bounded recent-wire history.
    Observation observe(const FrameIdentity &identity,
                        std::uint64_t observedMonotonicNs);

    // Receiver-side non-blocking publication.  try_lock is intentional: a
    // busy owner or a full queue is metadata loss, never receive backpressure.
    bool tryPublish(const Metadata &metadata) noexcept;
    std::size_t drain(std::vector<Metadata> &out,
                      std::size_t maxItems = static_cast<std::size_t>(-1));

    Observation observePublished(const Metadata &metadata);

    // Only an existing external/test oracle may mark a relative epoch as
    // confirmed.  No UI input is implied by this method.
    void confirmWithOracle(std::uint64_t absoluteStart);

    RoundDescriptor snapshot() const;
    std::uint64_t metadataDropped() const noexcept {
        return metadataDropped_.load(std::memory_order_relaxed);
    }
    bool hasLast() const noexcept { return hasLast_; }

private:
    struct SeenWire {
        std::uint16_t wire = 0;
        std::uint64_t expanded = 0;
    };

    void beginRound(RoundCloseReason reason, bool unknown);
    void remember(std::uint16_t wire, std::uint64_t expanded);
    bool findRemembered(std::uint16_t wire, std::uint64_t &expanded) const;

    Config config_;
    RoundDescriptor current_;
    bool hasLast_ = false;
    std::uint16_t lastWire_ = 0;
    std::uint64_t lastExpanded_ = 0;
    std::uint64_t relativeBase_ = 0;
    std::uint64_t lastObservedNs_ = 0;
    std::deque<SeenWire> recent_;
    mutable std::mutex metadataMutex_;
    std::deque<Metadata> metadataQueue_;
    std::atomic<std::uint64_t> metadataDropped_{0};
};
