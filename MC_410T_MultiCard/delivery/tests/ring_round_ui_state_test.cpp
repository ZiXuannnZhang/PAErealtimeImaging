// T6/T7/T8 — Physical-round UI state seam tests.
//
// Covers the extracted paimage::RingRoundUiState used by MainWindow for the
// unified physical-round business boundary:
//   T6  TimeoutBoundary resets per-round UI counters (frame/block)
//   T7  CountBoundary keeps the final-frame-driven reset (late old-round final
//       frame is still admitted and counted, then the frame counter resets)
//   T8  Stale pre-boundary reconstruction snapshots are rejected after a
//       TimeoutBoundary reset (no counter advance, no image pollution)
// plus the reset lifecycle (imaging start / assembler reconfigure).

#include "RingRoundUiState.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++g_failures;
    }
}

} // namespace

int main()
{
    using paimage::RingRoundUiState;

    // ── T6: TimeoutBoundary resets per-round UI counters ────────────────
    {
        RingRoundUiState ui;
        for (int i = 0; i < 3; ++i) {
            ui.recordSubmitIndex(static_cast<std::uint64_t>(i + 1));
            ui.onBlock();
        }
        for (int i = 0; i < 5; ++i) ui.onFrame();
        check(ui.frameCount() == 5, "T6 pre-timeout frame count");
        check(ui.blockCount() == 3, "T6 pre-timeout block count");

        ui.onTimeoutBoundary();
        check(ui.frameCount() == 0, "T6 timeout: frame count reset");
        check(ui.blockCount() == 0, "T6 timeout: block count reset");
        check(ui.snapshot().epoch == 1, "T6 timeout: epoch advanced");

        // New round counts from zero again.
        ui.onFrame();
        ui.recordSubmitIndex(4);
        ui.onBlock();
        check(ui.frameCount() == 1, "T6 new round frame count starts at 1");
        check(ui.blockCount() == 1, "T6 new round block count starts at 1");
    }

    // ── T7: CountBoundary keeps final-frame-driven reset ────────────────
    {
        RingRoundUiState ui;
        const int bpf = 5;
        // Round 0: 5 blocks submitted; the CountBoundary event fires when the
        // 5th logical identity is classified, BEFORE its reconstruction
        // snapshot returns.
        for (int s = 1; s <= 5; ++s) {
            ui.recordSubmitIndex(static_cast<std::uint64_t>(s));
            ui.onBlock();
        }
        ui.onCountBoundary();
        check(ui.frameCount() == 0, "T7 count boundary: no immediate reset side effect");

        // The late old-round final frame is admitted (no stale cutoff on the
        // count path), counted, and drives the frame-end reset itself.
        bool frameEnd = false;
        for (int s = 1; s <= 5; ++s) {
            check(ui.admitSnapshot(s), "T7 admit in-round snapshot");
            ui.onFrame();
            frameEnd = ui.noteSnapshot(s % bpf == 0);
            if (s < 5)
                check(!frameEnd, "T7 mid-round snapshot is not a frame end");
        }
        check(frameEnd, "T7 5th admitted snapshot completes the frame");
        check(ui.frameCount() == 5, "T7 final frame counted before reset");
        ui.resetFrameCount();
        check(ui.frameCount() == 0, "T7 frame counter reset at round end");

        // The fixture marks the last source block of each five-block round.
        // Production never derives this marker from admitted snapshot counts.
        for (int s = 6; s <= 10; ++s) {
            ui.recordSubmitIndex(static_cast<std::uint64_t>(s));
            ui.onBlock();
            ui.onFrame();
            frameEnd = ui.noteSnapshot(s % bpf == 0);
        }
        check(frameEnd, "T7 next frame end at the 10th admitted snapshot");
        check(ui.snapshot().blockCount == 10, "T7 block counter accumulates across count boundary");
    }

    // ── T8: stale pre-boundary reconstruction snapshots rejected ────────
    {
        RingRoundUiState ui;
        // Partial round: 2 blocks submitted, no completed frame (bpf=5).
        ui.recordSubmitIndex(1);    // producer identity 1
        ui.recordSubmitIndex(2);    // producer identity 2
        ui.onBlock();
        ui.onBlock();

        // TimeoutBoundary: UI counters reset immediately; stale cutoff armed
        // after ring_reset was handed to ImagingSvc.
        ui.onTimeoutBoundary();
        ui.armStaleCutoff(2);
        check(ui.frameCount() == 0, "T8 timeout: counters reset");
        check(ui.blockCount() == 0, "T8 timeout: block counter reset");

        // Stale snapshots produced from pre-reset blocks must be rejected.
        check(!ui.admitSnapshot(2), "T8 stale snapshot (submit_index=2) rejected");
        check(!ui.admitSnapshot(1), "T8 stale snapshot (submit_index=1) rejected");
        ui.noteStaleSnapshot();
        ui.noteStaleSnapshot();
        check(ui.staleSnapshotsDropped() == 2, "T8 stale drop counter");

        // New round blocks continue the producer submit_index (shm frame_seq
        // is not the admission identity and is not reset by ring_reset).
        for (int s = 3; s <= 7; ++s) {
            ui.recordSubmitIndex(static_cast<std::uint64_t>(s));
            ui.onBlock();
            check(ui.admitSnapshot(s), "T8 new-round snapshot admitted");
        }
        // Frame completes at the 5th admitted snapshot of the new round.
        bool end = false;
        for (int i = 0; i < 5; ++i) {
            ui.onFrame();
            end = ui.noteSnapshot(i == 4);
        }
        check(end, "T8 new round frame completes at 5 admitted snapshots");
        ui.resetFrameCount();
        check(ui.frameCount() == 0, "T8 new round frame counter reset at frame end");
        check(ui.snapshot().blockCount == 5, "T8 new round block count excludes stale round");
    }

    // ── Lifecycle reset (imaging start / assembler reconfigure) ─────────
    {
        RingRoundUiState ui;
        ui.recordSubmitIndex(1);
        ui.onBlock();
        ui.onFrame();
        ui.onTimeoutBoundary();
        ui.armStaleCutoff(1);
        ui.reset();
        const auto s = ui.snapshot();
        check(s.frameCount == 0 && s.blockCount == 0, "lifecycle: counters cleared");
        check(s.submissionsTotal == 0, "lifecycle: submission space cleared");
        check(s.epoch == 0 && s.staleSnapshotsDropped == 0, "lifecycle: epoch/drops cleared");
        check(ui.admitSnapshot(1), "lifecycle: stale cutoff disarmed after reset");
    }

    // ── B1..B6: producer submit_index identity domain -----------------
    {
        RingRoundUiState ui;
        ui.recordSubmitIndex(7);
        ui.onBlock();
        ui.onTimeoutBoundary();
        ui.armStaleCutoff(7);

        // B1: a failed local submit reserves no snapshot identity; it cannot
        // make the legal next identity stale.
        check(!ui.admitSnapshot(7), "B1 old producer identity rejected");
        check(ui.admitSnapshot(8), "B1 next legal producer identity admitted");

        // B2: a dontwait send gap (8 is absent; 9 is the next real identity)
        // is harmless because the cutoff remains in submit_index space.
        ui.armStaleCutoff(9);
        check(ui.admitSnapshot(10), "B2 submit_index gap does not drop new snapshot");

        const auto before = ui.snapshot();
        // B3: delayed old snapshots do not change any frame/block state.
        check(!ui.admitSnapshot(1), "B3 delayed old snapshot rejected");
        check(!ui.admitSnapshot(9), "B3 second delayed old snapshot rejected");
        ui.noteStaleSnapshot();
        ui.noteStaleSnapshot();
        const auto after = ui.snapshot();
        check(after.frameCount == before.frameCount &&
              after.blockCount == before.blockCount &&
              after.snapshotsThisRound == before.snapshotsThisRound,
              "B3 stale snapshots do not advance counters");

        // B4/B5: immediate new snapshots and a mixed old/new sequence are
        // admitted solely by the producer identity.
        check(ui.admitSnapshot(11), "B4 immediate new snapshot admitted");
        check(!ui.admitSnapshot(2), "B5 another old snapshot rejected");
        check(ui.admitSnapshot(12), "B5 new snapshot after old arrivals admitted");

        // B6: service/session lifecycle reset clears the old cutoff.
        ui.reset();
        check(ui.admitSnapshot(1), "B6 lifecycle reset accepts new identity 1");
    }

    // ── B7: CountBoundary remains final-frame driven ------------------
    {
        RingRoundUiState ui;
        for (std::uint64_t index = 1; index <= 2; ++index) {
            ui.recordSubmitIndex(index);
            ui.onBlock();
            ui.onFrame();
            ui.noteSnapshot(false);
        }
        const auto before = ui.snapshot();
        ui.onCountBoundary();
        const auto after = ui.snapshot();
        check(after.frameCount == before.frameCount &&
              after.blockCount == before.blockCount,
              "B7 CountBoundary does not reset before final frame");
        check(after.epoch == 0, "B7 CountBoundary does not advance timeout epoch");
    }

    if (g_failures == 0) {
        std::cout << "ring_round_ui_state_test: ALL PASS\n";
        return 0;
    }
    std::cout << "ring_round_ui_state_test: " << g_failures << " FAILURE(S)\n";
    return 1;
}
