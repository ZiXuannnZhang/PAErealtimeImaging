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
        for (int i = 0; i < 3; ++i) { ui.recordSubmit(); ui.onBlock(); }
        for (int i = 0; i < 5; ++i) ui.onFrame();
        check(ui.frameCount() == 5, "T6 pre-timeout frame count");
        check(ui.blockCount() == 3, "T6 pre-timeout block count");

        ui.onTimeoutBoundary();
        check(ui.frameCount() == 0, "T6 timeout: frame count reset");
        check(ui.blockCount() == 0, "T6 timeout: block count reset");
        check(ui.snapshot().epoch == 1, "T6 timeout: epoch advanced");

        // New round counts from zero again.
        ui.onFrame();
        ui.recordSubmit();
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
            ui.recordSubmit();
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
            frameEnd = ui.noteSnapshot(bpf);
            if (s < 5)
                check(!frameEnd, "T7 mid-round snapshot is not a frame end");
        }
        check(frameEnd, "T7 5th admitted snapshot completes the frame");
        check(ui.frameCount() == 5, "T7 final frame counted before reset");
        ui.resetFrameCount();
        check(ui.frameCount() == 0, "T7 frame counter reset at round end");

        // Next round continues the global admitted-snapshot space: the next
        // frame end lands on the 10th admitted snapshot (svc m_ringBlockIndex
        // is not reset at count boundaries either).
        for (int s = 6; s <= 10; ++s) {
            ui.recordSubmit();
            ui.onBlock();
            ui.onFrame();
            frameEnd = ui.noteSnapshot(bpf);
        }
        check(frameEnd, "T7 next frame end at the 10th admitted snapshot");
        check(ui.snapshot().blockCount == 10, "T7 block counter accumulates across count boundary");
    }

    // ── T8: stale pre-boundary reconstruction snapshots rejected ────────
    {
        RingRoundUiState ui;
        // Partial round: 2 blocks submitted, no completed frame (bpf=5).
        ui.recordSubmit();          // svc will publish snapshot seq 1
        ui.recordSubmit();          // svc will publish snapshot seq 2
        ui.onBlock();
        ui.onBlock();

        // TimeoutBoundary: UI counters reset immediately; stale cutoff armed
        // after ring_reset was handed to ImagingSvc.
        ui.onTimeoutBoundary();
        ui.armStaleCutoff();
        check(ui.frameCount() == 0, "T8 timeout: counters reset");
        check(ui.blockCount() == 0, "T8 timeout: block counter reset");

        // Stale snapshots produced from pre-reset blocks must be rejected.
        check(!ui.admitSnapshot(2), "T8 stale snapshot (seq=2) rejected");
        check(!ui.admitSnapshot(1), "T8 stale snapshot (seq=1) rejected");
        ui.noteStaleSnapshot();
        ui.noteStaleSnapshot();
        check(ui.staleSnapshotsDropped() == 2, "T8 stale drop counter");

        // New round blocks continue the svc-global seq (shm frame_seq is not
        // reset by ring_reset) and are admitted.
        for (int s = 3; s <= 7; ++s) {
            ui.recordSubmit();
            ui.onBlock();
            check(ui.admitSnapshot(s), "T8 new-round snapshot admitted");
        }
        // Frame completes at the 5th admitted snapshot of the new round.
        bool end = false;
        for (int i = 0; i < 5; ++i) {
            ui.onFrame();
            end = ui.noteSnapshot(5);
        }
        check(end, "T8 new round frame completes at 5 admitted snapshots");
        ui.resetFrameCount();
        check(ui.frameCount() == 0, "T8 new round frame counter reset at frame end");
        check(ui.snapshot().blockCount == 5, "T8 new round block count excludes stale round");
    }

    // ── Lifecycle reset (imaging start / assembler reconfigure) ─────────
    {
        RingRoundUiState ui;
        ui.recordSubmit();
        ui.onBlock();
        ui.onFrame();
        ui.onTimeoutBoundary();
        ui.armStaleCutoff();
        ui.reset();
        const auto s = ui.snapshot();
        check(s.frameCount == 0 && s.blockCount == 0, "lifecycle: counters cleared");
        check(s.submissionsTotal == 0, "lifecycle: submission space cleared");
        check(s.epoch == 0 && s.staleSnapshotsDropped == 0, "lifecycle: epoch/drops cleared");
        check(ui.admitSnapshot(1), "lifecycle: stale cutoff disarmed after reset");
    }

    if (g_failures == 0) {
        std::cout << "ring_round_ui_state_test: ALL PASS\n";
        return 0;
    }
    std::cout << "ring_round_ui_state_test: " << g_failures << " FAILURE(S)\n";
    return 1;
}
