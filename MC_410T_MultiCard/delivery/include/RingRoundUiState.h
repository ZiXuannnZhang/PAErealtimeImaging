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
// (recordSubmit), the UI thread (snapshot admission, boundaries), and tests.
//
// Stale snapshot model: ImagingSvc keeps a monotonic shm frame_seq that is NOT
// reset by ring_reset. On TimeoutBoundary we record the submission cut; any
// snapshot whose svc-global seq is at or below the cut was produced from
// pre-reset blocks and must not advance the new round's counters nor redraw
// the cleared image.
class RingRoundUiState {
public:
    struct Snapshot {
        std::uint64_t frameCount = 0;          // per-round output frame counter
        std::uint64_t blockCount = 0;          // per-round submitted block counter
        std::uint64_t snapshotsThisRound = 0;  // admitted Ring snapshots this round
        std::uint64_t submissionsTotal = 0;    // Ring blocks submitted (this state lifetime)
        std::uint64_t epoch = 0;               // TimeoutBoundary count applied
        std::uint64_t staleSnapshotsDropped = 0;
        std::uint64_t frameEndEvents = 0;      // snapshotsThisRound hit a bpf multiple
    };

    // Full reset on imaging start / assembler (re)configure.
    void reset();

    // One Ring block was submitted to ImagingSvc (assembler block callback).
    void recordSubmit();
    void onBlock();

    // Admit a Ring display snapshot carrying the svc-global sequence `seq`.
    // Returns false for a stale pre-boundary snapshot; the caller must drop it
    // (release the buffer) without counting or drawing.
    bool admitSnapshot(std::int64_t svcGlobalSeq);
    void noteStaleSnapshot();

    // Count one admitted snapshot. Returns true when it completes a frame,
    // i.e. admitted snapshots this round reached a multiple of blocksPerFrame.
    bool noteSnapshot(int blocksPerFrame);

    void onFrame();        // one output frame produced (m_imagingFrameCount)
    void resetFrameCount();

    // Round boundary inputs from PhysicalRoundNormalizer (data already decided).
    void onCountBoundary();    // counters keep running; frame-end reset stays
    // Immediate per-round UI reset. The stale cutoff is armed separately (see
    // armStaleCutoff) after ImagingSvc has processed ring_reset, so that
    // submissions racing in during boundary handling are classified exactly.
    void onTimeoutBoundary();
    // Arm the stale snapshot cut at the current submission total. Called on
    // the network thread after ring_reset was handed to ImagingSvc: the svc
    // consumes pre-reset blocks FIFO, so every block submitted up to this
    // point is pre-reset and any snapshot with svc seq <= the cut is stale.
    void armStaleCutoff();

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
    std::uint64_t staleCutoffSeq_ = 0;   // svc-global seq at last TimeoutBoundary
    bool hasStaleCutoff_ = false;
    std::uint64_t staleDropped_ = 0;
    std::uint64_t frameEnds_ = 0;
};

} // namespace paimage
