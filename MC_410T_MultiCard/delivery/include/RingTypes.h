#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Small, transport-independent contracts shared by acquisition, saving and
// ring imaging.  These types intentionally contain no Qt or STL objects that
// are placed in shared memory; the shared-memory ABI has its own fixed-width
// representation in ImagingSharedMemory.h.
enum class PositionConfidence : std::uint8_t {
    RelativeOnly = 0,
    Confirmed = 1,
    Unknown = 2
};

enum class RoundCloseReason : std::uint8_t {
    None = 0,
    IdleTimeout,
    ExplicitStop,
    SuspectedReset,
    HalfRangeAmbiguous,
    ConfigurationChanged,
    ServiceRestart
};

struct FrameIdentity {
    std::uint64_t measurementSession = 0;
    std::uint16_t wireTrigger = 0;
    std::uint64_t expandedTrigger = 0;
    int cardId = -1;
    std::uint64_t ingressId = 0;
    std::uint64_t firstReceiveMonotonicNs = 0;
    std::uint64_t configVersion = 0;
};

struct FrameQuality {
    std::uint32_t schemaVersion = 1;
    bool assemblyComplete = false;
    bool packetCoverageComplete = false;
    bool packetLengthValid = false;
    bool sampleLengthValid = false;
    bool sampleOriginKnown = false;
    bool qualityUnknown = true;
    std::uint32_t expectedPacketCount = 0;
    std::uint32_t receivedPacketCount = 0;
    std::uint64_t expectedPayloadBytes = 0;
    std::uint64_t actualPayloadBytes = 0;
    std::vector<std::uint8_t> packetCoverage;
    std::string missingReason;

    bool inputUsable() const noexcept {
        return !qualityUnknown && assemblyComplete && packetCoverageComplete &&
               packetLengthValid && sampleLengthValid;
    }
};

struct RoundDescriptor {
    std::uint64_t roundId = 0;
    std::uint64_t candidateStart = 0;
    std::uint64_t candidateEnd = 0;
    std::string basis = "first-visible-relative";
    std::uint32_t expectedCount = 4001;
    PositionConfidence positionConfidence = PositionConfidence::RelativeOnly;
    RoundCloseReason closeReason = RoundCloseReason::None;
    bool unknownEpoch = false;
};

struct RingBlock {
    std::uint64_t serviceGeneration = 0;
    std::uint64_t roundId = 0;
    std::uint64_t configVersion = 0;
    std::uint64_t blockSeq = 0;
    std::uint64_t startPosition = 0;
    std::uint32_t positionCount = 0; // 1..50; raw remains fixed-width/padded
    std::uint64_t validPositionBits = 0;
    std::uint32_t channelCount = 0;
    std::uint32_t sampDepth = 0;
    bool wavelengthAssumed = true;
    PositionConfidence positionConfidence = PositionConfidence::RelativeOnly;
    FrameQuality quality;
    std::vector<float> raw;
    std::vector<float> anglesDeg;
    std::vector<std::uint8_t> channels;
    std::vector<std::uint8_t> wavelengths;

    bool positionValid(std::uint32_t position) const noexcept {
        return position < 64 && (validPositionBits & (std::uint64_t(1) << position)) != 0;
    }
};
