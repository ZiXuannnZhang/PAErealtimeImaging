#pragma once

#include <cstdint>
#include <mutex>

namespace paimage {

// Per-round Ring UI state for the physical-round business boundary.
//
// Owns the per-round output-frame / submitted-block counters, the snapshot
// admission epoch, and frame-end detection for the Ring display path. This
// logic used to be inlined in MainWindow; it is extracted so the boundary
// semantics (TimeoutBoundary immediate reset, CountBoundary final-frame-driven
// reset, stale pre-boundary snapshot rejection) are unit-testable without a UI.
//
// Threading: internally synchronized. Called from the assembler worker
// (recordSubmitIndex), the UI thread (snapshot admission, boundaries), and tests.
//
// Stale snapshot model: the producer assigns a monotonic submit_index to each
// ring_block_ready message and ImagingSvc echoes that identity in
// ring_snapshot_ready. On TimeoutBoundary the cutoff is taken in this same
// producer-index domain; svc frame_seq remains display/diagnostic metadata.
class RingRoundUiState {
public:
    struct Snapshot {
        std::uint64_t frameCount = 0;          // per-round output frame counter
        std::uint64_t blockCount = 0;          // per-round submitted block counter
        std::uint64_t snapshotsThisRound = 0;  // admitted Ring snapshots this round
        std::uint64_t submissionsTotal = 0;    // recorded producer identities
        std::uint64_t lastSubmitIndex = 0;     // producer submit_index high-water mark
        std::uint64_t staleCutoffSubmitIndex = 0;
        bool hasStaleCutoff = false;
        std::uint64_t epoch = 0;               // TimeoutBoundary count applied
        std::uint64_t staleSnapshotsDropped = 0;
        std::uint64_t frameEndEvents = 0;      // snapshotsThisRound hit a bpf multiple
    };

    // Full reset on imaging start / assembler (re)configure.
    void reset();

    // Record the producer identity after submitRingBlock reserved it.  The
    // value is retained even when the nonblocking command send fails: the
    // resulting gap is safe and must never be confused with a frame count.
    void recordSubmitIndex(std::uint64_t submitIndex);
    void onBlock();

    // Admit a Ring display snapshot carrying the producer `submit_index`.
    // Returns false for a stale pre-boundary snapshot or an absent identity;
    // the caller must drop it (release the buffer) without counting/drawing.
    bool admitSnapshot(std::uint64_t submitIndex);
    void noteStaleSnapshot();

    // Count one admitted snapshot. Returns true when it completes a frame,
    // i.e. admitted snapshots this round reached a multiple of blocksPerFrame.
    bool noteSnapshot(int blocksPerFrame);

    void onFrame();        // one output frame produced (m_imagingFrameCount)
    void resetFrameCount();

    // Round boundary inputs from PhysicalRoundNormalizer (data already decided).
    void onCountBoundary();    // counters keep running; frame-end reset stays
    // Immediate per-round UI reset. The stale cutoff is armed separately (see
    // armStaleCutoff) after the producer's ring_reset command has been sent,
    // so submissions racing in during boundary handling are classified in the
    // same submit_index domain.
    void onTimeoutBoundary();
    // Arm an explicit producer submit_index cutoff after ring_reset was sent.
    void armStaleCutoff(std::uint64_t cutoffSubmitIndex);
    void disarmStaleCutoff();

    Snapshot snapshot() const;
    std::uint64_t frameCount() const;
    std::uint64_t blockCount() const;
    std::uint64_t staleSnapshotsDropped() const;

private:
    mutable std::mutex mutex_;
    std::uint64_t frameCount_ = 0;
    std::uint64_t blockCount_ = 0;
    std::uint64_t snapshotsThisRound_ = 0;
    std::uint64_t submissionsTotal_ = 0;
    std::uint64_t epoch_ = 0;
    std::uint64_t lastSubmitIndex_ = 0;
    std::uint64_t staleCutoffSubmitIndex_ = 0;
    bool hasStaleCutoff_ = false;
    std::uint64_t staleDropped_ = 0;
    std::uint64_t frameEnds_ = 0;
};

} // namespace paimage
