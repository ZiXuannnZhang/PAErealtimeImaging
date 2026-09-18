# Handoff — Session C, 2026-09-17

## Baseline / final state

```text
Session B base SHA    = 676df4a946fed56b903474354984bff44ab50875
Session C code commit = 2f844449aaf28666bbd8c21556b83900e3f814b5
Session C docs commit = receipt + handoff commit on top (see receipt)
branch                = codex/session-c-timeout-variable-round-20260917-214638
final remote HEAD     = reported in the final execution report (out of band)
```

## Frozen production behavior

- **disable=false (count boundary)**: unchanged Session A semantics — logical
  0..N-1, index N-1 final with one-shot `roundComplete`, generation++,
  next round restarts the startup filter.
- **disable=true (imaging cap)**: the configured count is only a realtime
  imaging cap. Logical 0..N-1 keep raw+sync/display/Ring/CUDA; index >=N
  keeps raw save only until the timeout boundary. The cap uses the shared
  classification (multi-card shared slots), is not a modulo, and reopens at
  index 0 after each timeout (new generation).
- **Active idle timeout owner**: the 20 ms `NetworkController::pollPaimage()`
  poll chain (`Backend::poll` -> `HostOutput::pollPhysicalRoundTimeout` ->
  `PhysicalRoundNormalizer::timeoutBoundaryIfIdle`) is the single production
  owner. Check+transition are atomic inside the Normalizer mutex; no UI timer
  or wall-clock snapshot participates. Works in any state with at least one
  distinct trigger, including `AwaitingControl` partial-startup rounds.
- **Partial-startup timeout**: X=7 with 3 distinct triggers closes at 3 and
  latches `lastCompleted{Physical,StartupFiltered}DistinctCount=3`.
- **Timeout AutoSave binding**: `commitAutoSaveBoundary(..., Timeout, ...)`
  completes synchronously before the boundary callback returns; queued old
  writes keep the old sessionGen (no FileSaver drain); a missing new binding
  fails closed.
- **Capture-before-reset**: the old-round PNG payload (immutable pixels +
  ranges) is captured before `ImagingBypass`/`RingBlockAssembler`/
  `sendRingReset` run; the disk write happens after reset on a thread pool.
  Auto-save writes to `oldDirectory/recon_png`; manual mode keeps
  `m_reconSaveDir`.
- **Ring residual discard**: `resetAfterPhysicalTimeout()` discards pending
  lines/partial phase and never synthesizes a partial block; late old
  identity fails closed/stale-drops with diagnostics.
- **Reset fail-closed**: `ring_reset` submit success arms the stale
  submit-index cutoff; failure latches snapshot admission blocked until
  lifecycle reconfiguration.
- **No synthetic completion**: early/variable-length rounds never claim
  reconstruction completeness; `expectedBlocks`, CUDA geometry and angle
  modulo are untouched; raw saving never stops at the configured count in
  disable mode.

## Changed files (code commit)

```text
MC_410T_MultiCard/delivery/include/ImagingDisplayWindow.h
MC_410T_MultiCard/delivery/include/MainWindow.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/Backend.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/HostOutput.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/PhysicalRoundNormalizer.h
MC_410T_MultiCard/delivery/include/TimeoutPresentation.h          (new)
MC_410T_MultiCard/delivery/src/ImagingDisplayWindow.cpp
MC_410T_MultiCard/delivery/src/MainWindow.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/HostOutput.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/PhysicalRoundNormalizer.cpp
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tests/count_boundary_save_binding_test.cpp
MC_410T_MultiCard/delivery/tests/paimage_core/physical_round_normalizer_test.cpp
MC_410T_MultiCard/delivery/tests/paimage_host_output_test.cpp
MC_410T_MultiCard/delivery/tests/timeout_presentation_test.cpp   (new)
```

## Session D inputs

Session D performs integration review / validation / necessary bugfix only.
It must not redesign Session A/B/C semantics: keep the count-boundary
contract, the disable-mode imaging cap, the single poll-based timeout
owner, the synchronous AutoSave timeout binding, capture-before-reset
ordering and the fail-closed reset admission. The remaining open item is
the real hardware window: verify CountBoundary/TimeoutBoundary, the
~4007-trigger round behavior, directory rollover and the timeout
screenshot chain from live FPGA/NIC data. `ring_svc_selftest` remains
without a real `--data` acquisition file in this repository.
