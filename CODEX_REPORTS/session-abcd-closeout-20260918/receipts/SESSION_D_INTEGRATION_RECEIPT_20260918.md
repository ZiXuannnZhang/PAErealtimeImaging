# Session D integration execution receipt — 2026-09-18

## Task document

`TASKS/SessionD_集成审查与候选交付验证_20260918-000458.md` on `origin/codex/task-docs` (fetched 2026-09-18, tip 621492d).

## Git identity

```text
repository   = ZiXuannnZhang/PAErealtimeImaging (https://github.com/ZiXuannnZhang/PAErealtimeImaging)
branch       = codex/session-d-integration-validation-20260918-000458
starting SHA = d9daa2d7af6bb8341349e405433824e89e42bcd4
              (Session C independently reviewed remote HEAD; verified equal to
               origin/codex/session-c-timeout-variable-round-20260917-214638
               before branch creation; remote HEAD had not moved)
candidate code SHA = d9daa2d7af6bb8341349e405433824e89e42bcd4
                     (no production/test code bugfix found in Session D;
                      this commit adds receipt/handoff/checklist docs only)
bugfix commits     = none
final remote HEAD  = reported out-of-band in the final execution report
                     (per repo rule: no self-referential final SHA inside docs commits)
tracked git status = clean before this docs commit
                    (git status --porcelain --untracked-files=no empty)
```

Branch creation was performed in the session-c worktree
(`D:/ChatGPT/PAERealtimeImaging/_worktrees/session-c`) at the exact starting
SHA; the worktree carries the retained ignored build/CUDA runtime dependencies
listed below.

## Part I/II — Integration audit (task sections 5-9)

Static audit executed with `rg` over the exact candidate tree before any
build/test. No frozen-contract violation was found; no source change was made.

### Section 8 — production path owner verification

1. **PAimage production ingress** — PASS, single owner.
   `Backend::Backend` (`src/PaimageAcquisition/Backend.cpp:9-46`) wires
   `SocketReceiver` callbacks directly into `HostOutput`:
   `receiver_(..., [this](Frame f){output_.card(f);}, [this](auto t,const auto& f,bool startup){output_.sync(t,f,startup);})`.
   `SocketReceiver` owns the embedded `SourceCore core_`
   (`include/PaimageAcquisition/SocketReceiver.h:83`). Production frames flow
   SocketReceiver -> SourceCore -> HostOutput::card/sync
   (`src/PaimageAcquisition/HostOutput.cpp:50,83`).
2. **Full missing-trigger gap accounting** — PASS, single owner.
   `paimage::SourceCore` emits `Decision::TriggerGap` (count = delta-1) at the
   per-card assembly activation point (`src/PaimageAcquisition/SourceCore.cpp:116`);
   `NetworkControllerPaimage.cpp:182-183` observationSink adds it to
   `CardStats::missingTriggerCount` and packet-equivalent drops. Threshold
   semantics (wrap-forward, backstep <256 no-op, >=256 same-session reset
   recovery re-anchor, session-boundary anchor isolation) are regression-covered
   by `paimage_production_gap_test` (B-ADD-1..8) and `data_processor_batch_test`
   (legacy B1-B4).
3. **Startup filter / count policy / timeout — shared PhysicalRoundNormalizer** —
   PASS. HostOutput constructs the single normalizer and forwards policy
   (`src/PaimageAcquisition/HostOutput.cpp:27-42`; `include/PaimageAcquisition/HostOutput.h:67-68`);
   `Backend::Settings` carries `startupFilterTriggerCount`/`disableCountBoundary`
   (`include/PaimageAcquisition/Backend.h:16-21`); controller setters at
   `src/NetworkController.cpp:698-707` reach `HostOutput` only.
4. **Active physical-idle timeout — 20 ms poll chain, one boundary owner** —
   PASS. `NetworkControllerPaimage.cpp:268` (20 ms QTimer ->
   `NetworkController::pollPaimage` :470) -> `Backend::poll`
   (`include/PaimageAcquisition/Backend.h:28-32`) ->
   `HostOutput::pollPhysicalRoundTimeout` (`include/PaimageAcquisition/HostOutput.h:52-54`)
   -> `PhysicalRoundNormalizer::timeoutBoundaryIfIdle`
   (`src/PaimageAcquisition/PhysicalRoundNormalizer.cpp:256`).
   `RingBlockAssembler`'s standalone timeout is disabled in production
   (`setTimeoutManagedExternally(true)` at `src/MainWindow.cpp:595`; internal
   gate `if (!m_timeoutManagedExternally && ...)` `src/RingBlockAssembler.cpp:279`).
5. **Raw save — HostOutput card path, uncapped** — PASS.
   `HostOutput::card` (`src/PaimageAcquisition/HostOutput.cpp:50-76`) resolves
   the save generation via the coordinator resolver, tags the frame
   (fail-closed sentinel on lookup miss) and pushes to `workers_.pushCard`;
   `consumeCard` delivers via `processors_[card]->deliverAssembled(group,true,false)`
   into the direct-save sink. No logical-index or cap filter exists on this path.
6. **Realtime cap — HostOutput sync path only** — PASS.
   `HostOutput::sync` (`src/PaimageAcquisition/HostOutput.cpp:83-96`) filters
   startup-control frames, stale classifications of a timed-out round
   (`session==timeoutSession_ && generation<timeoutGeneration_`), and — only
   when `disableCountBoundary()` — logical indices
   `>= configuredLogicalTriggersPerRound` before `workers_.pushSync`.
7. **AutoSave round binding — (measurementSession, roundGeneration)** — PASS.
   TimeoutBoundary branch commits synchronously inside the boundary callback:
   `m_netController->commitAutoSaveBoundary(event.measurementSession,
   event.roundGeneration, AutoSaveBoundaryKind::Timeout, ...)`
   (`src/MainWindow.cpp:2381-2387`); the card path resolves directories via
   `saveSessionResolver_` (`src/PaimageAcquisition/HostOutput.cpp:66-73`).
   Coordinator: `include/PaimageAcquisition/AutoSaveRoundCoordinator.h`,
   `src/PaimageAcquisition/AutoSaveRoundCoordinator.cpp`.
8. **Ring hard barrier / timeout residual discard** — PASS.
   `RingBlockAssembler` identity fields/acceptRound/staleRoundDrops
   (`include/RingBlockAssembler.h:45-64,154-201`); timeout reset path
   `resetAfterPhysicalTimeout` (`src/RingBlockAssembler.cpp:101`) invoked
   exactly once per (session, generation) under `m_ringAssemblerMutex` with the
   boundary dedup (`src/MainWindow.cpp:2402-2412`).
9. **CUDA reset / submit-index stale cutoff** — PASS.
   `MainWindow.cpp:624-629` registers the assembler timeout callback that sends
   `ImagingController::sendRingReset` (`src/ImagingController.cpp:397`) and
   records `m_ringResetCommandSent`; the boundary reset lambda
   (`src/MainWindow.cpp:2412-2418`) applies
   `TimeoutPresentation::applyResetResult` (`include/TimeoutPresentation.h:52-59`):
   success arms `RingRoundUiState::armStaleCutoff(ringLastSubmitIndex())`
   (`src/RingRoundUiState.cpp:117`); failure latches
   `m_ringSnapshotAdmissionBlocked` (store sites: `src/MainWindow.cpp:709`
   svcStopped -> blocked; :747 svcReady -> re-armed; :5425 lifecycle reset).
10. **Timeout old-presentation capture-before-reset** — PASS.
    `TimeoutPresentation::close` (`include/TimeoutPresentation.h:34-53`)
    captures the immutable pixel/range writer BEFORE invoking the reset lambda;
    MainWindow calls it inside the assembler mutex
    (`src/MainWindow.cpp:2405-2412`), renders/writes the PNG on QThreadPool
    after the reset (`src/MainWindow.cpp:2419-2421`). Auto-save mode writes to
    `oldDirectory/recon_png`; manual mode keeps the manual recon dir.
11. **No synthetic completion** — PASS. `expectedBlocks` /
    `reconstructionComplete` / `sourceRoundComplete` owners remain
    `RingBlockAssembler`, `ImagingController`, `ImagingSvc` and
    `RingReconCompletion`; no HostOutput/MainWindow path sets or rewrites them
    (rg over the candidate tree: no new synthetic producer; early/variable
    rounds assert no block / no completion in `ring_production_blockers_test`
    T1/M2 and `ring_block_assembler_test` R1-R3).
12. **Session B UI/settings single source** — PASS. Persistence helper
    `include/RoundPolicySettings.h` (group `RingConfigDialog/Defaults`, keys
    `startupFilterTriggerCount`, `disableCountBoundary`); only consumers are
    `RingConfigDialog.cpp:392,440` (restore/save-defaults chain) and
    `MainWindow.cpp:2300-2302` (startup load -> controller setters);
    Apply/OK signal path `MainWindow.cpp:4395-4396`. No second store exists.

### Section 9 — duplicate owner / legacy reactivation search

- **MainWindow 250 ms timeout-save owner** — none. No `m_ringTimeoutTimer` /
  `ringTimeoutSaveDone` references remain in `src/MainWindow.cpp` /
  `include/MainWindow.h` (only unrelated 250 MHz sampling constants).
- **RingBlockAssembler standalone timeout in production** — disabled
  (`setTimeoutManagedExternally(true)`, `src/MainWindow.cpp:595`); the
  internal idle timer is gated off (`src/RingBlockAssembler.cpp:279`).
- **Production misroute through DataProcessor packet-assembly gap accounting** —
  none. `DataProcessor::processInputBatch` is only reachable from the
  legacy/test packet-assembly path (`src/DataProcessor.cpp:208,379`);
  production PAimage frames assemble in `SourceCore` and deliver through
  `deliverAssembled`, so production never double-counts gaps.
- **Second startup filter/count policy store** — none. Single
  `RoundPolicySettings` helper; rg over src/include shows no other QSettings
  group or hardcoded policy duplicate.
- **UI self-computed physical round count** — none. UI reads
  `NetworkController::physicalRoundSnapshot()` only
  (`src/MainWindow.cpp:2500,3484,5408`) through
  `CardStatusFormatting::roundDisplayForUi` current->lastCompleted fallback
  (`include/CardStatusFormatting.h:21-23`).
- **Raw save cap** — none. The card path has no index/cap gate; cap exists only
  in `HostOutput::sync` under `disableCountBoundary`.
- **Hardcoded 7 / 4007 as protocol truth** — none. rg for `4007|4006` over
  `src/include` returns nothing; X=7 appears only as a test/registry-driven
  configuration value.
- **New CountBoundary observer clearing the Ring data plane** — none. The
  MainWindow CountBoundary branch only records diagnostics and calls
  `m_roundUi.onCountBoundary()`; ring frame reset stays
  `deferred_to_final_frame` (`src/MainWindow.cpp:2357-2376`).
- **Old sync/presentation re-entering a new round after timeout** — blocked on
  all planes: stale sync filter (`src/PaimageAcquisition/HostOutput.cpp:87-88`),
  Ring identity stale-drop + service identity floor
  (`include/RingBlockAssembler.h:64`, `RingReconRoundState`), producer
  submit-index stale cutoff (`src/RingRoundUiState.cpp:117-127`), delayed old
  UI conversion rejection (`include/TimeoutPresentation.h:23`).

No duplicate owner was found. No bugfix was required under task section 12.

## Part III — Policy matrix and variable-round scenarios

### P1-P4 (deterministic matrix, N=4)

Executed by `paimage_host_output_test` (C1-C3/C8/C13 four-combo loop,
N=4, startup in {0,7} x disable in {false,true}); PASS lines captured:

```text
PASS C1-C3/C8/C13 startup=0 disable=0 raw=4 realtime=4/card
PASS C1-C3/C8/C13 startup=0 disable=1 raw=6 realtime=4/card
PASS C1-C3/C8/C13 startup=7 disable=0 raw=4 realtime=4/card
PASS C1-C3/C8/C13 startup=7 disable=1 raw=6 realtime=4/card
```

Per combo the test asserts: startup filtered count (first N distinct filtered),
logical indices 0..3 shared across both cards, physical distinct count via
snapshot, CountBoundary count (disable?0:1), TimeoutBoundary count 0 before the
poll, generation (disable?0:1), final-marker one-shot behavior
(finals==(disable?0:1), multi-card suppression documented), raw saved count
(logical = disable?6:4, actual file bytes checked), realtime accepted count
(4 per card), and next-round restart (C8: index 0, generation 1, startup filter
restarts). P1=startup0/disable0, P2=startup7/disable0, P3=startup0/disable1,
P4=startup7/disable1 — all PASS.

### D1-D5 (variable-round scenarios)

- **D1 (early timeout before N)** — PASS. Normalizer:
  `physical_round_normalizer_test` C4-C7 block (N=4, disable=true,
  startup=0: three logical identities below N, active poll closes the round,
  latches lastCompleted counts, generation 1, repeated poll no-op, next trigger
  index 0). Raw/save: `paimage_host_output_test` T12/T7 (partial round sealed
  at 2 raw triggers into its own file, next round to a new file sequence,
  rollover metadata old gen0/2). Ring/no-synthesis:
  `ring_production_blockers_test` T1 (partial round emits no block; old
  identity stale-dropped; new round completes cleanly at its own source end).
- **D2 (timeout exactly at N, no CountBoundary)** — PASS. Normalizer A8
  (N=4 disable=true: index 3 asserts !roundComplete && !isFinalLogicalTrigger,
  countBoundaryResets==0, generation stays 0 through index 5) + A9 (timeout-only
  reset closes the over-counted window, generation 1, next round index 0);
  host-output disable combos assert counts==0/finals==0/generation==0 across
  index N-1 and beyond, and C8 poll produces exactly one TimeoutBoundary — the
  configured count itself never boundaries; timeout does.
- **D3 (over cap then timeout)** — PASS. `paimage_host_output_test` C2/C8
  disable combos: logical 0..N+1 (6 triggers, N=4) — raw saves all 6 (file bytes
  verified 6*16*2), realtime only indices 0..3 per card, exactly one timeout
  boundary, next round cap reopens at index 0 generation 1.
- **D4 (partial startup timeout, X=7, 3 distinct)** — PASS. Normalizer C5/C4
  block startup=7 (lastCompletedPhysical=3/lastCompletedFiltered=3 latched,
  next visible identity is operational control, filter restarts) and A10
  (explicit-timeout variant, same latches + next-startup-slot re-filter).
- **D5 (late old data after timeout)** — PASS. Host output C8 (late old sync
  suppressed, no reopen, raw keeps old stamp); binding test C8/C9 (queued old
  writes keep the old directory; new round resolves the new binding immediately);
  ring T1 (old identity stale on both planes + submit-index stale cutoff holds);
  ring_round_identity R7/R9 (delayed old snapshot stale, keeps its own old
  directory, does not consume a presentation transition); timeout presentation
  C10 (delayed old UI conversion rejected; capture owns pixels after live
  presentation destruction).

## Part IV — Automated regression (run from
`D:/ChatGPT/PAERealtimeImaging/_worktrees/session-c/MC_410T_MultiCard/delivery`
with `D:/Qt/Qt6.8.0/6.8.0/mingw_64/bin` prepended to PATH)

### Test-tree configure + build

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
# configure exit 0 (known CMAKE_OBJECT_PATH_MAX=200 long-path hashing notices)
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe' --build build/tests_all_mingw_debug -j 8
# build exit 0 ([15/15] relink-only delta on the identical Session C tree)
```

### Focal tests (all exit 0)

```text
.\build\tests_all_mingw_debug\paimage_core\physical_round_normalizer_test.exe
  -> PASS physical round normalizer contract (T1-T18, A1-A12, C4-C7)
.\build\tests_all_mingw_debug\paimage_host_output_test.exe
  -> PASS C1-C3/C8/C13 x four combos + production boundary line
.\build\tests_all_mingw_debug\round_policy_settings_test.exe -> PASS round policy settings
.\build\tests_all_mingw_debug\card_status_formatting_test.exe -> PASS card status formatting
.\build\tests_all_mingw_debug\data_processor_batch_test.exe -> PASS DataProcessor batch/counter tests
.\build\tests_all_mingw_debug\paimage_production_gap_test.exe
  -> PASS paimage production trigger-gap accounting: B-ADD-1..8
.\build\tests_all_mingw_debug\network_diagnostics_test.exe --legacy-control-only
  -> PASS 4/5-target loops (ctest invocation flag per tests/CMakeLists.txt:123)
.\build\tests_all_mingw_debug\timeout_presentation_test.exe
  -> PASS C10 capture-before-reset/old-directory/lifetime; C12 reset success/failure
.\build\tests_all_mingw_debug\ring_block_assembler_test.exe
  -> PASS all RingBlockAssembler tests (T1-T12,R1-R4,R10)
.\build\tests_all_mingw_debug\ring_round_identity_test.exe -> ALL PASS (R5-R10,R14)
.\build\tests_all_mingw_debug\ring_production_blockers_test.exe -> failures=0
.\build\tests_all_mingw_debug\count_boundary_save_binding_test.exe -> C1-C10 ALL PASS
.\build\tests_all_mingw_debug\filesaver_round_boundary_test.exe -> ALL PASS
.\build\tests_all_mingw_debug\auto_save_round_coordinator_test.exe -> ALL PASS
```

### Full CTest

```powershell
Set-Location build/tests_all_mingw_debug
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/ctest.exe' --output-on-failure -j 4
```

Result: **100% tests passed, 0 tests failed out of 45** — Total Test time
24.20 s (candidate code SHA d9daa2d7af6bb8341349e405433824e89e42bcd4).

Environment note (recorded per task section 14): the first full-suite run
executed inside the managed sandbox produced 44/45 with
`physical_round_normalizer_test` SEGFAULT (0.32 s) and 18 unkillable ghost
processes; the same binary passed standalone and in serial ctest re-runs, and
rapid-spawn loops inside the sandbox left additional ghost processes even for
runs that passed. This matches the child-spawn corruption class already
documented in the Session C receipt (PATH stripping for spawned children). The
authoritative full-suite result above was executed outside the sandbox on the
same tree; no product/test code change was made or required.

## Part V — Windows build and Ring/CUDA selftest

### Canonical Windows build

```powershell
Set-Location "D:/ChatGPT/PAERealtimeImaging/_worktrees/session-c/MC_410T_MultiCard/delivery"
cmd /c build_mingw_debug.cmd
```

Result: **exit 0**. First execution re-ran preset configure at the candidate
SHA and relinked `main.cpp.obj`/`PAimageReceiverDiagnostics.exe`
(2026-09-18 00:43:01, regenerating BuildIdentity for the candidate tree); a
captured re-run on the same tree exited 0 with `ninja: no work to do` and the
script's four-exe checks passed. Tracked tree clean at build time.

Core products (`build/mingw_debug/bin`):

```text
PAimageReceiverDiagnostics.exe  65,302,127 bytes  SHA256 E7752E8D256EBFE7D91CA926E4473083FCB47C9CD9DBAAF721D56E568F6B2926
ImagingSvc.exe                   3,337,427 bytes  SHA256 93B99CE5EDBB49C39AD664DABE7481C1DBF9487DB9C571FBE953982E8D8EDB02
ring_svc_selftest.exe            1,941,820 bytes  SHA256 787B8747974847D03C8EE2963D4EC56F120A77FCD6CBD57CEB5AA7A3A9A49922
ring_udp_replay.exe                333,547 bytes  SHA256 47233B1771BD20E453A663A5C56A082E41458D9626588A166BA90FDD3A2FDCD5
```

### Ring/CUDA software selftest — PASS (upgraded from Session C NOT RUN)

Non-destructive re-check found a traceable, format-compatible acquisition
dataset that Session C had missed:

- file: `D:/ChatGPT/PAERealtimeImaging/testdata/14.dat` (265,600,000 bytes)
- SHA256: `FBCBC105343D8CAF8D1B231A93CF00CDBF4710F94A19B93B9F1A86F812E9E056`
- provenance: documented in `_migration_pack/HANDOFF.md` ("testdata/14.dat 是
  float64 列主序（每列一根 A-line），14.dat=8300 列×4000 点"; original field
  path `D:/zzx/data/20260519/14.dat`); matches the selftest reader exactly
  (`sampDepth = 4000`, column-major doubles, `src/RingReconCuda/ring_svc_selftest.cpp:63-71,147`).
  File is gitignored (testdata/ in .gitignore) but retained locally with
  documented origin.

```powershell
.\build\mingw_debug\bin\ring_svc_selftest.exe --data D:/ChatGPT/PAERealtimeImaging/testdata/14.dat `
  --svc .\build\mingw_debug\bin\ImagingSvc.exe --id 14 --grid-mm 0.1 --block 200 `
  --channels 255 --sector-start 180 --out <build>/selftest_session_d_out
# exit 0; done: channels=8 K=500 rounds=2 blocks=10 avg=67.38 ms
# [RingSHMObs] notifications=10 consumed=10 mismatch=0 ready_zero=0 duplicate=0 gap=0

.\build\mingw_debug\bin\ring_svc_selftest.exe --data D:/ChatGPT/PAERealtimeImaging/testdata/14.dat `
  --svc .\build\mingw_debug\bin\ImagingSvc.exe --id 14 --grid-mm 0.1 --block 200 `
  --channels 255 --sector-start 180 --identity-jump-at 3 --out <build>/selftest_session_d_out_idjump
# exit 0; [barrier-test] fresh-round max_pixel_difference=0; SHM observability clean
```

Both runs drove the real `ImagingSvc.exe` (CUDA ring reconstruction) on the
candidate build. This is a software selftest on retained ring data; it is NOT
FPGA/NIC/LabVIEW hardware validation.

## Part VI — Delivery staging

```text
staging path: D:/ChatGPT/PAERealtimeImaging/_worktrees/session-c/artifacts/build-delivery/20260918-005157_d9daa2d/
contents: bin/ (43 files, full copy of build/mingw_debug/bin),
         build-manifest.txt, git-receipt.txt, dependency-sha256.txt,
         file-sha256.txt, validation.txt, build.log
```

Key dependency hashes and provenance (`dependency-sha256.txt`):

```text
pa_recon_core.dll         C1A37E73147C1B8250F96CCDC1FCFC08CA28CECBF1CB3C046D4AB426F2EA0893  repository-tracked (delivery/libs/imaging)
cufft64_12.dll            2480D8AB849D7E9A375275F6C0278B8764C14AC0C1A3BDACAF256AE4A93C5590  retained local CUDA 12 runtime (untracked; hash identical to Session A's recorded provenance)
ring_recon_cuda.dll       BF40472D5A46363A35DD1084A01A8C15F13EF203F3ED5B4BB5EDF767EEEC14B5  _migration_pack/prebuilt_cuda/bin (build-script fallback; no local CUDA rebuild)
cudart64_12.dll           C2C9A9C22A9BCBA90E261825968836787B331038047A26770CFFB7A583C28344  _migration_pack/prebuilt_cuda/bin
libzmq-v141-mt-4_3_5.dll  37610023D91951BC4177DB1F96B54B28911976AC70B221A852887709A02A9AD3  repository-tracked (third_party/zeromq/bin)
import libraries: pa_recon_core.lib (repository-tracked), libring_recon_cuda.dll.a (_migration_pack/prebuilt_cuda/bin) — hashes in dependency-sha256.txt
```

`file-sha256.txt` covers every file in the staged `bin/` recursively (43
entries). Critical runtime provenance is traceable (CUDA runtime from the
retained migration-pack baseline and the retained CUDA 12 cufft whose hash
matches the Session A receipt).

```text
SESSION_D_DELIVERY_CANDIDATE = READY
```

## Part VII — Hardware boundary

`HARDWARE_VALIDATION_CHECKLIST_SESSION_D_20260918.md` is preparation for the
next real FPGA/NIC/LabVIEW window. Session D did not initiate any hardware
measurement; all hardware acceptance items remain PENDING and the checklist is
not a hardware PASS record.

## Result summary

```text
SESSION_A_SOFTWARE_REVIEW                 = APPROVE (independently reviewed)
SESSION_B_SOFTWARE_REVIEW                 = APPROVE (independently reviewed, incl. two addenda)
SESSION_C_SOFTWARE_REVIEW                 = APPROVE (independently reviewed)
SESSION_D_INTEGRATION_REVIEW              = PASS
SESSION_D_AUTOMATED_TESTS                 = PASS (focal 14/14; full ctest 45/45)
SESSION_D_WINDOWS_BUILD                   = PASS (exit 0, 4 core products)
SESSION_D_RING_CUDA_SELFTEST              = PASS (default + identity-jump, real ImagingSvc, traceable dataset)
SESSION_D_DELIVERY_CANDIDATE              = READY
PHYSICAL_ROUND_HARDWARE_VALIDATION        = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS            = NOT PROVEN
MAIN_INTEGRATION                          = NOT PERFORMED / USER DECISION REQUIRED
```
