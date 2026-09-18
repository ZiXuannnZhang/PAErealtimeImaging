# Session C execution receipt — 2026-09-17

## Task document

`TASKS/SessionC_超时分轮与实时成像上限_20260917-214638.md`
on `origin/codex/task-docs` (fetched 2026-09-17).

## Repository / branch / exact baseline

```text
repository   = git@github.com:ZiXuannnZhang/PAErealtimeImaging.git
branch       = codex/session-c-timeout-variable-round-20260917-214638
starting SHA = 676df4a946fed56b903474354984bff44ab50875
               (Session B independently reviewed remote HEAD; verified at start)
code commit  = 2f844449aaf28666bbd8c21556b83900e3f814b5
               (Session C: active physical-idle timeout round closure,
                realtime imaging cap, capture-before-reset screenshots;
                15 files, +374/-116)
docs commit  = this commit (receipt + handoff only; per repo rule no
               binary rebuild for pure documentation commits)
```

## Active physical-idle timeout owner (final path)

```text
NetworkController::m_paimageTimer (20 ms)
  -> NetworkController::pollPaimage()
  -> Backend::poll()                       // SocketReceiver::now() steady clock
  -> HostOutput::pollPhysicalRoundTimeout(now)
  -> PhysicalRoundNormalizer::timeoutBoundaryIfIdle(now)
```

- Idle check + boundary transition execute atomically inside the Normalizer
  mutex (no snapshot->force-boundary TOCTOU). Notification runs outside the
  state lock but inside `HostOutput::normalizationMutex_`, which also
  serializes `card`/`sync`/`consumeSync`, so boundary commit, old-image
  capture and Ring/CUDA reset can never interleave with classification or
  downstream enqueue of either round.
- The poll is independent of imaging / recon-PNG / auto-save enablement and
  of `AwaitingControl` vs `CollectingScan`: any round with at least one
  distinct trigger closes on idle, including partial startup (X=7, 3 seen).
- The explicit test seam `timeoutBoundary(...)` keeps its semantics;
  `MainWindow::m_ringTimeoutTimer` and `m_ringTimeoutSaveDone` are retired
  (no UI/wall-clock timeout owner remains);
  `RingBlockAssembler::setTimeoutManagedExternally(true)` kept.

## Realtime imaging cap (exact gate)

```text
HostOutput::sync(trigger, frames, startup):
  shared PhysicalRoundNormalizer::classify per frame      // Session A identity
  startup-filtered                                   -> return
  cached classification of a timed-out round
    (session == timeoutSession && generation < timeoutGeneration) -> return
  disableCountBoundary && decision == LogicalScan
    && logicalTriggerIndex >= configuredLogicalTriggersPerRound -> return // cap
  workers_.pushSync(...)
```

- Cap exists only when `disableCountBoundary=true`; the card/raw/save path is
  never capped (raw continues past N until the timeout boundary).
- `disableCountBoundary=false` CountBoundary behavior is untouched (index
  N-1 final, one-shot `roundComplete`, generation++, startup filter restart).
- The cap reopens automatically after a timeout because the new generation
  restarts classification at logical index 0 (no mutable frozen state).
- One caveat documented for reviewers: a multi-card shared boundary trigger
  carries `roundComplete` on exactly one group (the frame first classified
  as newDistinct); cached late cards get the frozen one-shot suppression
  while `isFinalLogicalTrigger` stays stable (asserted in
  `paimage_host_output_test`).

## Timeout AutoSave binding path

`TimeoutBoundary` is the source-side save authority:
`commitAutoSaveBoundary(event.measurementSession, event.roundGeneration,
AutoSaveBoundaryKind::Timeout, ...)` runs synchronously inside the boundary
callback before it returns. Because classification and `saveSessionResolver`
run under the same `normalizationMutex_`, the first new-round `card()`
can only resolve its directory after the new binding is published. Queued
old-round writes keep their old `sessionGen` (no synchronous FileSaver
drain); a missing new binding fails closed to the sentinel and never falls
back to the old directory (Session A/B contract, C9 evidence below).

## Screenshot capture-before-reset ordering

```text
TimeoutBoundary callback (source thread):
  1. commitAutoSaveBoundary(...)                // synchronous, old+new directory
  2. under m_ringAssemblerMutex + (session,generation) dedup:
     m_timeoutPresentation.close(session, nextGeneration, autoSave, oldDirectory,
         reset = [ImagingBypass::clear(StaleSession);
                  RingBlockAssembler::resetAfterPhysicalTimeout();
                  ImagingController::sendRingReset();
                  arm stale submit cutoff / fail-closed admission latch])
       -> captures the UI-published immutable PNG writer (pixels/ranges)
          BEFORE invoking reset
  3. PNG render/write on QThreadPool AFTER reset (async disk I/O allowed)
  4. queued UI update via QMetaObject::invokeMethod(...)
```

- Auto-save mode writes to `oldDirectory/recon_png` from the boundary commit;
  manual/non-auto-save mode keeps `m_reconSaveDir`; a missing auto-save old
  directory cannot reuse the new UI target (no job, fail-closed).
- Deterministic evidence: `timeout_presentation_test` fixes the order
  `boundary -> capture -> reset -> write`, the old-directory target, payload
  lifetime (capture owns pixels after live presentation destruction) and the
  manual/missing-directory branches (C10), plus reset success/failure
  admission results (C12).

## Ring/CUDA reset path

- `resetAfterPhysicalTimeout()` clears pending lines/partial block phase
  without any synthetic partial block; residuals remain in diagnostics.
- Reset exactly once per `(measurementSession, roundGeneration)` via the
  boundary dedup under `m_ringAssemblerMutex`.
- `sendRingReset()` success arms the producer submit-index cutoff
  (`armStaleCutoff(ringLastSubmitIndex())`); failure latches
  `m_ringSnapshotAdmissionBlocked` — old CUDA snapshots can no longer be
  admitted into the new round (C12; later successes do not clear the latch).
- No synthetic completion anywhere: `reconstructionComplete`,
  `sourceRoundComplete`, `expectedBlocks`, CUDA geometry and angle modulo
  are unchanged; early/variable-length rounds never become "complete".

## Deterministic tests C1-C13

All focal scenarios live in the standard test tree; the commands below run
from `MC_410T_MultiCard/delivery` with Qt 6.8.0 mingw_64 + MinGW 13.1.0
prepended to `PATH`.

| Test | Scenarios | Result |
|---|---|---|
| `paimage_core/physical_round_normalizer_test` | C4 active poll without next trigger; C5 partial startup X=7/3 latch; C6 poll-wins / next-trigger-wins exactly-once; C7 late card no anchor refresh, no second boundary; disabled/backward-clock no-ops | PASS |
| `paimage_host_output_test` | C1 count-enabled compatibility; C2 cap with disable=true (raw 0..5 saved, sync/display 0..3 only, actual raw file bytes past cap); C3 startup+cap multicard shared slots; C8 cap reopen after timeout (index 0, generation 1, late old sync suppressed); C13 four policy combos startup x disable | PASS |
| `count_boundary_save_binding_test` | C9 active timeout publishes binding before next trigger; queued old writes keep old dir; new round resolves new dir | PASS |
| `timeout_presentation_test` | C10 capture-before-reset, old directory, payload lifetime, manual mode, missing-directory; C12 reset success/failure fail-closed | PASS |
| `ring_block_assembler_test`, `ring_production_blockers_test`, `ring_round_identity_test` | C11 residual discard, no synthetic partial block, stale identity fail-closed | PASS |

Per-combo evidence from `paimage_host_output_test` stdout:

```text
PASS C1-C3/C8/C13 startup=0 disable=0 raw=4 realtime=4/card
PASS C1-C3/C8/C13 startup=0 disable=1 raw=6 realtime=4/card
PASS C1-C3/C8/C13 startup=7 disable=0 raw=4 realtime=4/card
PASS C1-C3/C8/C13 startup=7 disable=1 raw=6 realtime=4/card
```

## Test-tree configure and build (exact commands)

```powershell
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe' -S tests -B build/tests_all_mingw_debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe `
  -DQt6_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6 `
  -DQt6CoreTools_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6CoreTools `
  -DQt6GuiTools_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6GuiTools `
  -DQt6WidgetsTools_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6WidgetsTools `
  -DCMAKE_OBJECT_PATH_MAX=200 `
  -DPython3_EXECUTABLE=C:/Users/yyps/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe' --build build/tests_all_mingw_debug -j 8
```

- Configure exit 0; build exit 0 with [290/290] targets, no errors.
- `-DCMAKE_OBJECT_PATH_MAX=200` was added because the default object paths
  in this shorter worktree produced a 264-character MinGW depfile path
  (`...`data_processor_imaging_isolation_test.dirD_`...`ImagingBypass.cpp.obj.d`),
  exceeding the Windows MAX_PATH 260 limit and failing the compile; the value
  makes CMake hash long object paths (identical to what CMake did
  automatically in the longer Session B worktree). No project files changed.
- Environment note (test host only): the sandboxed shell intermittently
  strips the prepended PATH for spawned child processes, so Qt-DLL tests
  failed with `0xc0000135` when launched via ctest. The runtime set
  `Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll, libgcc_s_seh-1.dll,
  libstdc++-6.dll, libwinpthread-1.dll` was therefore copied into
  `build/tests_all_mingw_debug/` (the loader searches the executable
  directory first). These are ignored build-directory files, not repo
  changes; the deployed production binaries come from the standard build
  below instead.

## Full CTest suite

```powershell
Set-Location build/tests_all_mingw_debug
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/ctest.exe' --output-on-failure -j 4
```

Result: **100% tests passed, 0 tests failed out of 45**
(Total Test time 24.73 s). Includes the required Session A/B regressions:
`physical_round_normalizer_test`, `paimage_host_output_test`,
`round_policy_settings_test`, `card_status_formatting_test`,
`data_processor_batch_test`, `paimage_production_gap_test`,
`network_diagnostics_test`, `ring_block_assembler_test`,
`ring_round_identity_test`, `ring_production_blockers_test`,
`count_boundary_save_binding_test`, `filesaver_round_boundary_test`,
`auto_save_round_coordinator_test`.

One test expectation was corrected against frozen Session A semantics:
the new C1/C2 check initially required `roundComplete` on both cards of a
multi-card boundary trigger, but `roundComplete` is the frozen one-shot
boundary signal (cached late cards are suppressed; the first-classified
frame keeps the marker via the converter merge), so exactly one group
carries it; the test now asserts `finals==(disable?0:1)` with that
rationale. No production code was changed for this.

## Windows build (BUILD_STANDARD)

Run at the exact code commit with a clean tracked tree:

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging\_worktrees\session-c\MC_410T_MultiCard\delivery"
cmd /c build_mingw_debug.cmd
```

Result: **exit 0** (preset `mingw-debug`, configure + build + product checks +
windeployqt). Core products in `build/mingw_debug/bin/`:

```text
PAimageReceiverDiagnostics.exe  65,302,127 bytes  SHA256 3617B3F0F5EB111B35459ED66C0ADFE8AB9A510D4B34D1317F05B28CBCF81EDE
ImagingSvc.exe                   3,337,427 bytes
ring_svc_selftest.exe            1,941,820 bytes
ring_udp_replay.exe                333,547 bytes
```

The first full run (same tree) completed [61/61]; the committed-tree
verification re-run relinked only `main.cpp.obj`/`PAimageReceiverDiagnostics.exe`
and re-deployed up-to-date Qt DLLs.

## ring/CUDA software selftest

`build/mingw_debug/bin/ring_svc_selftest.exe` was built and its CLI verified.
End-to-end execution: **NOT RUN** — the tool requires a real acquisition data
file (`--data <11.dat>`) that does not exist in this repository, the
`_migration_pack`, or any recorded historical run on any branch. This is a
software-only session; reset-related behavior is covered by C11/C12 and the
ring assembler/identity/production-blocker regressions above. It is NOT
claimed as FPGA/NIC/LabVIEW validation.

## Dependency sources (per BUILD_STANDARD scope)

```text
Qt 6.8.0 mingw_64            D:/Qt/Qt6.8.0/6.8.0/mingw_64
MinGW 13.1.0                 D:/Qt/Qt6.8.0/Tools/mingw1310_64
CMake + Ninja                D:/Qt/Qt6.8.0/Tools/CMake_64, .../Tools/Ninja
ZeroMQ                       delivery/third_party/zeromq (preset-managed)
ring_recon_cuda.dll          _migration_pack/prebuilt_cuda/bin (untracked local)
libring_recon_cuda.dll.a     _migration_pack/prebuilt_cuda/bin (untracked local)
cufft64_12.dll               copied from the Session B worktree's
                             MC_410T_MultiCard/delivery/libs/imaging
                             (untracked local, listed per BUILD_STANDARD 2.1)
```

`ring_recon_cuda` and `cufft64_12.dll` are ignored local dependencies; their
hash policy follows BUILD_STANDARD's untracked-dependency listing rule.

## Tracked git status / local-remote parity

- After the code commit the tracked working tree was clean
  (`git status --porcelain --untracked-files=no` empty).
- Push via the local git credential manager:
  `git -c credential.helper=manager push -u origin codex/session-c-timeout-variable-round-20260917-214638`.
  Post-push verification `git rev-parse HEAD` equals
  `git ls-remote origin refs/heads/codex/session-c-timeout-variable-round-20260917-214638`;
  the final remote HEAD is reported in the execution report.

## Result summary

```text
SESSION_C_SOFTWARE_IMPLEMENTATION = PASS
SESSION_C_AUTOMATED_TESTS         = PASS (ctest 45/45)
SESSION_C_WINDOWS_BUILD           = PASS (exit 0, 4 core products)
SESSION_C_RING_CUDA_SELFTEST      = NOT RUN (missing real --data acquisition file)
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS     = NOT PROVEN BY SESSION C
```
