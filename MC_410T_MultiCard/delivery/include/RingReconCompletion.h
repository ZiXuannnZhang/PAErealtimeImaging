#pragma once

#include <cstdint>

namespace paimage {

// Service-side completion semantics for one physical round of Ring
// reconstruction.
//
// sourceRoundComplete: the data plane reported that the final logical trigger
// of the physical round was observed. It decides lifecycle closure only: the
// round is closed and the accumulator reset once it arrives, even when the
// reconstruction is incomplete. The next round never "tops up" the old one.
//
// reconstructionComplete: sourceRoundComplete AND the service consumed exactly
// the configured number of complete Ring blocks for this round. This alone
// authorizes the final-image business action (ring_snapshot_ready
// round_complete, final PNG, presentation completion).
struct RingReconCompletion {
    bool sourceRoundComplete = false;
    bool reconstructionComplete = false;
    bool blockCountMismatch = false;
    std::int64_t blocksConsumed = 0;
    std::int64_t expectedBlocks = 0;
};

// Fail closed on any count drift: exact equality with the configured block
// count is the only complete state. Overconsumption must not pass with >=.
inline RingReconCompletion evaluateRingReconCompletion(bool sourceRoundComplete,
                                                       std::int64_t blocksConsumed,
                                                       std::int64_t expectedBlocks) {
    RingReconCompletion result;
    result.sourceRoundComplete = sourceRoundComplete;
    result.blocksConsumed = blocksConsumed;
    result.expectedBlocks = expectedBlocks;
    result.reconstructionComplete = sourceRoundComplete &&
        expectedBlocks > 0 && blocksConsumed == expectedBlocks;
    result.blockCountMismatch = sourceRoundComplete && !result.reconstructionComplete;
    return result;
}

} // namespace paimage
