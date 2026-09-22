# 独立 Frontend Preprocessing Stage 架构落地：构建与回归验证回执

## 1. Task / source identity

Task document:

```text
origin/codex/task-docs:TASKS/独立FrontendPreprocessingStage架构落地_20260922-031200.md
```

```text
repository                = ZiXuannnZhang/PAErealtimeImaging
branch                    = codex/frontend-preprocessor-stage-20260922
STARTING_SHA              = 9938269220fcaf8e81216820f6bb46549ca59888
IMPLEMENTATION_SOURCE_SHA = 5af8c39021b62ba046512f6d19480794b4a27b00
REPORT_HEAD               = evidence-only receipt commit (see section 12)
```

Entry-state check (task section 2) passed before any edit:

```text
HEAD == 9938269220fcaf8e81216820f6bb46549ca59888   YES
git status --porcelain --untracked-files=no        empty
```

No merge / rebase / cherry-pick of `main` or any other branch entered this task:

```text
git merge-base 9938269220fcaf8e81216820f6bb46549ca59888 HEAD
  = 9938269220fcaf8e81216820f6bb46549ca59888
```

### Progress taken over

The previous execution agent had completed branch preparation only:

```text
local branch    codex/frontend-preprocessor-stage-20260922  = 9938269 (exact start)
remote branch   origin/codex/frontend-preprocessor-stage-20260922 = 9938269 (exact start)
Codex worktree  ~/.codex/worktrees/frontend-preprocessor-stage-20260922 = 9938269, tracked clean
implementation  none (no source, test or build-input change existed)
```

Everything below is new work performed in this session.

## 2. Scope actually implemented

Task 1 only: architecture plus identity processing.

```text
INCLUDED  per-card asynchronous Frontend Preprocessing Stage
INCLUDED  frontend-owned deep copy of the raw TriggerGroup
INCLUDED  processFrontendSignal() as a single identity insertion point
INCLUDED  full-resolution display preparation moved into the stage
INCLUDED  frontend queue admission result type, separate from ImagingSubmitResult
INCLUDED  session / TimeoutBoundary stale barrier
INCLUDED  lifecycle ownership in NetworkController
INCLUDED  thread-safe per-card stage Snapshot + minimal query interface
EXCLUDED  Butterworth coefficient design
EXCLUDED  SOS biquad
EXCLUDED  forward/backward filtering (filtfilt)
EXCLUDED  MATLAB historical crop / system_delay rules
EXCLUDED  linear-imaging legacy filter changes
EXCLUDED  Ring reconstruction algorithm changes
EXCLUDED  save file format / raw save content changes
EXCLUDED  display downsampling re-introduction
EXCLUDED  filter UI or QSettings key
EXCLUDED  performance tuning / GPU work
EXCLUDED  unrelated refactoring
```

No filter numeric result is claimed anywhere in this receipt; filter design is not
part of Task 1.

## 3. FrontendPreprocessor ownership / lifecycle summary

New files:

```text
MC_410T_MultiCard/delivery/include/FrontendPreprocessor.h
MC_410T_MultiCard/delivery/src/FrontendPreprocessor.cpp
```

Data flow now:

```text
raw TriggerGroup
├─ FileSaver / raw save          -> raw, not through the frontend stage
├─ FramePublisher                -> raw, consumed before frontend submit
└─ FrontendPreprocessor::submit(raw const)
      ↓ bounded FIFO / worker
      ↓ deep-copy frontend-owned TriggerGroup
      ↓ processFrontendSignal()            // identity in Task 1
      ↓ full-resolution display preparation
      ├─ DisplayBuffer::update / updateFullRes
      └─ RingFeedSink / ImagingBypass
```

Ownership rules enforced:

- `submit(const TriggerGroupPtr&)` treats the input as const raw. The worker
  builds `std::make_shared<TriggerGroup>(*raw)`; the raw object is never mutated
  by the stage. Metadata (`measurementSession`, `roundGeneration`, `triggerSeq`,
  `logicalTriggerIndex`, `roundComplete`, `isFinalLogicalTrigger`,
  `normalizationApplied`, `physicalDecision`, `sessionGen`, `sourceTimedOut`, …)
  and full-resolution `freqA`/`freqB` are preserved verbatim by the deep copy.
- Raw save and `FramePublisher` keep consuming the original raw group.
- `DataProcessor` no longer owns or calls `DisplayBuffer` / `RingFeedSink`, and no
  longer writes `*_display` on the shared raw group. `prepareDisplayData()` moved
  to the stage and runs on the frontend clone only. Production has exactly one
  frontend dispatch owner: `FrontendPreprocessor`.
- DisplayBuffer and RingFeedSink consume the same frontend clone from one
  preprocessing result (not two independent results).

Lifecycle:

```text
owner                = NetworkController (std::vector<std::unique_ptr<FrontendPreprocessor>> m_frontendStages)
instances            = one worker instance per acquisition card
start                = startPaimage(): stage created and start()ed before
                       createPaimageBackend(), i.e. before any submit can occur
stop (listen stop)   = upstream submit stopped first (Backend/OutputWorkers joined),
                       then stopFrontendStages() joins and clears the stages, then
                       m_displayBuffers / Ring sink dependencies are destroyed
rollback / failure   = startPaimage() failure path and NetworkController::rollbackStart()
                       both call stopFrontendStages() before DisplayBuffer destruction
destructor           = FrontendPreprocessor::~FrontendPreprocessor() calls stop()
terminate            = never used as a normal stop mechanism for this stage
```

## 4. Queue capacity / policy

```text
structure       std::deque<Item> + mutex + condition_variable
policy          bounded FIFO, per-card trigger order preserved
                (explicitly NOT a latest-only queue)
default capacity 256 entries per card (FrontendPreprocessor::kDefaultQueueCapacity)
                  ≈ 1.3 s of 200 Hz triggers per card
capacity is a constructor parameter; tests use 2 and 8192 to exercise bounds
submit          O(1) enqueue only. The queue lock is held only for the enqueue and
                is never held across the deep copy, display preparation or Ring
                dispatch, so submit never waits for the worker.
overflow        FrontendSubmitResult::QueueFull, counted in Snapshot.queueRejected
```

Note on non-blocking admission: the first draft used `std::try_to_lock` for
`submit`. Under tight producer/consumer loops that rejected real frames as
`QueueBusy` purely on lock contention, which would drop A-lines from the Ring for
no capacity reason. Since the critical section is O(1) and never spans worker
work, contention is now resolved in bounded time instead of being turned into a
drop. `FrontendSubmitResult::QueueBusy` is kept in the type as a reserved value
and is not produced by the current implementation. Task test 5 (worker parked in
a test seam) proves `submit` / `deliverAssembled` still return without waiting for
the worker.

## 5. Session + timeout stale barrier implementation point

Stale predicate (`FrontendPreprocessor::staleLocked`, evaluated with the queue
lock held) rejects a frame when:

```text
sessionClosed_ or stopping_                                  -> stale
activeSession != 0 and frame.measurementSession != 0
  and frame.measurementSession != activeSession              -> stale
frame.roundGeneration < minDispatchableRoundGeneration
  and NOT (frame.isFinalLogicalTrigger
           and frame.roundGeneration + 1 == minDispatchable)  -> stale
```

The predicate is applied twice per frame, in the order frozen by task 3.3:

```text
dequeue raw const → stale check → deep copy → processFrontendSignal()
→ full-resolution display preparation → stale check again
→ DisplayBuffer update / updateFullRes → RingFeedSink
```

The second check is what makes an in-flight frame (already dequeued when the
boundary moves) droppable before dispatch.

Production wiring reuses the existing facts; no independent round inference was
introduced:

| boundary | production source | stage effect |
|---|---|---|
| measurement session start | `Backend::startMeasurement` → `HostOutput::beginSession(s)` → `FrontendSessionSink` | `beginSession(s)`: purge pending, `activeSession=s`, `minDispatchableRoundGeneration=0`, reopen dispatch |
| measurement stop / disarm | `Backend::stopMeasurement` → `HostOutput::endSession()` → `FrontendSessionSink(0)` → `NetworkController::invalidateFrontendStages()`; also `NetworkController::resetProcessorsAfterSession` | `endSession()`: purge pending and close dispatch so no old frame is delivered after stop |
| PhysicalRound TimeoutBoundary | `PhysicalRoundNormalizer::classify` / `timeoutBoundaryIfIdle` event → `HostOutput` normalizer observer → `FrontendBarrierSink(session, roundGeneration)` | `advanceRoundBarrier(session, roundGeneration)`: raises `minDispatchableRoundGeneration`, purges queued old-round frames, in-flight old-round frames die at the second stale check |
| PhysicalRound CountBoundary | deliberately not forwarded to the barrier | none. The just-completed final logical trigger keeps its own `roundGeneration` (the generation it completed) and must still dispatch exactly once. It is additionally exempt from the round-generation barrier so a barrier that has already advanced past its round cannot drop it. |

Barrier monotonicity: `advanceRoundBarrier` only moves forward
(`roundGeneration <= minDispatchableRoundGeneration` is a no-op) and ignores
events for a different measurement session.

## 6. HostOutput / DataProcessor async result semantics changes

`DataProcessor::DeliveryResult` no longer pretends to report a synchronous Ring
result:

```text
BEFORE  save, saveAccepted, displayAccepted, imagingAccepted,
        publisherAccepted, exception, imagingDropReason (ImagingSubmitResult)
AFTER   save, saveAccepted,
        frontendAccepted, frontendSubmit (FrontendSubmitResult),
        publisherAccepted, exception
```

- `frontendSubmit` / `frontendAccepted` describe **frontend queue admission
  only**. `FrontendSubmitResult` is a distinct enum from `ImagingSubmitResult`
  (asserted by `static_assert` in `frontend_preprocessor_test` T6).
- The asynchronous Ring outcome (`Accepted` / `QueueFull` / `QueueBusy` /
  `Disabled` / …) stays with `ImagingBypass` / Ring statistics. The stage records
  it only under explicitly downstream-marked counters
  (`downstreamRingAttempts/Accepted/Rejected/Exceptions`).
- HostOutput stage-7 trace now records frontend enqueue results instead of
  masquerading as the final Ring result:

```text
stage 7 reason 0 = save consumer result        (DeliveryResult::Save)
stage 7 reason 1 = frontend enqueue accepted   (0/1)          [was: display accepted]
stage 7 reason 2 = frontend submit result      (FrontendSubmitResult) [was: imaging result]
stage 7 reason 3 = publisher returned          (0/1)
stage 7 reason 4 = exception
stage 7 reason 5 = session stale after conversion
```

- Frontend queue rejection is never counted into UDP packet loss
  (`packetsDropped`), `missingTriggerCount`, `saveQueueDiscards` or
  `triggersDiscarded`. Verified directly by `frontend_preprocessor_test` T7.
- The legacy `flushAssemblyBuf` → `deliverAssembled(group, true, true)` path keeps
  raw save and frontend submit isolated: the save sink sees the unmodified raw
  group (empty `*_display`, original `freqA`/`freqB`) even while the frontend
  queue is full or rejecting.

## 7. Frontend Snapshot fields

`FrontendPreprocessor::Snapshot` (thread-safe; query per card through
`NetworkController::frontendSnapshot(int cardIdx)`, no UI added):

```text
accepted / enqueued              frontend queue admissions
processed                        frames that completed the whole frontend path
queueRejected                    bounded-queue rejections (QueueFull, reserved QueueBusy)
staleDropped                     stale-barrier / session / stop drops
exceptionDropped                 contained frontend-processing or downstream faults
currentDepth                     live FIFO depth
maxDepth                         peak FIFO depth
queueCapacity                    configured bound

activeSession                    current measurement session
minDispatchableRoundGeneration   current round stale barrier
deepCopies                       frontend-owned deep copies created
displayUpdates                   DisplayBuffer dispatches

downstreamRingAttempts           ─┐
downstreamRingAccepted            │ explicitly downstream Ring results,
downstreamRingRejected            │ never mixed into ingress or save loss
downstreamRingExceptions         ─┘
```

## 8. Future filter configuration contract (task section 5)

```text
high-pass cutoff frequency : adjustable   (NOT implemented in Task 1)
high-pass order            : adjustable   (NOT implemented in Task 1)
low-pass cutoff frequency  : adjustable   (NOT implemented in Task 1)
low-pass order             : adjustable   (NOT implemented in Task 1)
sample rate                : production FPGA_ADC_FREQ_HZ = 250 MHz
future UI input location   : RingConfigDialog "成像参数" dialog
```

Task 1 added none of: the four UI controls, parameter persistence, parameter
validation, or any parameter in `RingReconCudaConfig` / `ImagingSvc`
reconstruction config.

`FrontendPreprocessor::processFrontendSignal(TriggerGroup&)` is deliberately a
single, clearly marked, currently-identity insertion point. Future filtering acts
exactly once there, so a filter configuration update does not require rebuilding
the acquisition chain. `prepareDisplayData()` runs strictly after it and only
consumes the processed signal.

## 9. Build result + BuildIdentity

Full evidence in `build-summary.txt`. Summary:

```text
Build target branch      : codex/frontend-preprocessor-stage-20260922
Build target SHA         : 5af8c39021b62ba046512f6d19480794b4a27b00
Tracked tree clean       : yes (before the receipt commit)
Configure preset         : mingw-debug
Build preset             : mingw-debug-build
Build script             : cmd /c ".\build_mingw_debug.cmd"
Build result             : PASS (exit 0, 0 errors)
Qt / MinGW / CMake / Ninja : Qt 6.8.0 mingw_64 / GNU 13.1.0 / 3.30.5 / 1.12.1
Native bin directory     : MC_410T_MultiCard/delivery/build/mingw_debug/bin
Core binaries            : PAimageReceiverDiagnostics.exe, ImagingSvc.exe,
                           ring_svc_selftest.exe, ring_udp_replay.exe  (all PRESENT)
Ring CUDA source         : local build (build/ring_recon_cuda); _migration_pack fallback NOT used
Delivery directory       : NOT_CREATED (no field delivery package requested by this task)
Delivery ZIP             : NOT_CREATED
```

BuildIdentity generated at configure time on the final production/test source
commit:

```text
PAIMAGE_GIT_SHA          = 5af8c39021b62ba046512f6d19480794b4a27b00
PAIMAGE_TRACKED_DIRTY    = false
PAIMAGE_BUILD_TYPE       = Debug
PAIMAGE_COMPILER         = GNU 13.1.0
PAIMAGE_PRODUCT_BASELINE = e66a29bfbbaa6534911a23d48e8624fb8d552bec
PAIMAGE_SOURCE_SHA256    = 2b2f4a8b82ff49fc35c2d3d96fe3f999a0dcc403144f39ef520c5af5cea9a6ec
```

`PAIMAGE_GIT_SHA == IMPLEMENTATION_SOURCE_SHA` and `PAIMAGE_TRACKED_DIRTY == false`.

Key runtime dependency SHA-256 (full list in `build-summary.txt`):

```text
pa_recon_core.dll    c1a37e73147c1b8250f96ccdc1fcfc08ca28cecbf1cb3c046d4ab426f2ea0893
cufft64_12.dll       2480d8ab849d7e9a375275f6c0278b8764c14ac0c1a3bdacaf256ae4a93c5590
ring_recon_cuda.dll  bf40472d5a46363a35dd1084a01a8c15f13ef203f3ed5b4bb5edf767eeec14b5
cudart64_12.dll      c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344
libzmq-v141-mt-4_3_5.dll 37610023d91951bc4177db1f96b54b28911976ac70b221a852887709a02a9ad3
```

Build cache handling: the pre-existing `build/mingw_debug` CMakeCache had been
created under the historical workspace root `d:/ChatGPT/PAERealtimeImaging` and
refused the current root. Per `BUILD_STANDARD` section 6 only the git-ignored
`build/mingw_debug` and stale `build/tests_*` subdirectories were removed and
re-configured; `build/ring_recon_cuda` (the prebuilt Ring CUDA source) was kept.
No reset / rebase / source deletion was used to clear a cache.

## 10. Targeted tests: each result

Command and raw output in `ctest-targeted.txt`.

```text
frontend_preprocessor_test              PASS   (new)
paimage_host_output_test                PASS
data_processor_imaging_isolation_test   PASS
data_processor_batch_test               PASS
imaging_bypass_test                     PASS
ring_block_assembler_test               PASS
ring_round_identity_test                PASS
session_boundary_test                   PASS
session_boundary_receiver_test          PASS
filesaver_round_boundary_test           PASS
count_boundary_save_binding_test        PASS
timeout_presentation_test               PASS

TOTAL: 12/12 PASS, 0 FAIL, 0 SKIP, 0 NOT_RUN  (exit 0, 10.78 s)
```

New `frontend_preprocessor_test` covers the twelve required contracts:

```text
 1 identity deep-copy      PASS  output pointer != input pointer; metadata/freqA/freqB
                                 identical; raw object unchanged; 1 deep copy per frame
 2 full-resolution         PASS  2048-point (>1000) input kept in full by display and
                                 frontend output; raw *_display not produced
 3 one result / two consumers PASS DisplayBuffer and RingFeedSink observe the SAME
                                 frontend clone (pointer identity) and it differs from
                                 the raw group; deepCopies == 1
 4 FIFO ordering           PASS  64 triggers dispatched in per-card submit order
 5 non-blocking submit     PASS  with the worker parked in a test seam, submit and
                                 deliverAssembled return in <50 ms and processed stays 0
 6 bounded overflow        PASS  capacity-2 FIFO deterministically returns
                                 FrontendSubmitResult::QueueFull and queueRejected == 1;
                                 FrontendSubmitResult is not ImagingSubmitResult
 7 save isolation          PASS  12/12 raw saves consumed while the frontend queue
                                 rejected 10 frames; raw saved payload not rewritten;
                                 packetsDropped/missingTriggerCount/saveQueueDiscards
                                 all stay 0
 8 session stale           PASS  after beginSession(2) the queued and the in-flight
                                 frame of session 1 are staleDropped; session 2 passes
 9 timeout stale           PASS  after advanceRoundBarrier(session, 1) the queued and the
                                 in-flight round-0 frame are staleDropped; round 1 passes
10 CountBoundary final     PASS  the just-completed final logical trigger survives the
                                 boundary barrier and dispatches exactly once; the
                                 non-final old-round frame is dropped
11 stop lifecycle          PASS  stop() clears the queue and joins deterministically with
                                 processed == 0 (no post-stop dispatch); submit after stop
                                 returns Stopping; start/stop is safe again
12 exception containment   PASS  a frontend processing fault and a downstream Ring callback
                                 fault are both contained; all 6 raw saves succeed and the
                                 acquisition thread keeps running
```

## 11. Full CTest total / PASS / FAIL / SKIP / NOT_RUN

Command and raw output in `ctest-full.txt`.

```text
ctest -N                          -> Total Tests: 44, exit 0
ctest --output-on-failure -j 4    -> exit 0, 23.28 s

total   = 44
PASS    = 44
FAIL    = 0
SKIP    = 0
NOT_RUN = 0
```

No one-time diagnostic rerun was required: the parallel full run was green on its
first invocation.

## 12. Evidence directory and receipt-only diff

```text
evidence directory = CODEX_REPORTS/frontend-preprocessor-stage-20260922/
  validation-receipt.md
  build-summary.txt
  ctest-targeted.txt
  ctest-full.txt
  git-receipt.txt
```

All five files are text. No build tree, executable, DLL or archive is committed.

```text
REPORT_HEAD diff against IMPLEMENTATION_SOURCE_SHA
  = CODEX_REPORTS/frontend-preprocessor-stage-20260922/ only
```

Because a commit cannot contain its own SHA, `REPORT_HEAD` is reported in the
final task report and verified afterwards with `git rev-parse HEAD` and
`git diff --name-only 5af8c39021b62ba046512f6d19480794b4a27b00..HEAD`.

## 13. Test adaptations made to existing DataProcessor / HostOutput tests

Only interface-driven adaptations; no production assertion was weakened.

1. `DataProcessor` construction sites drop the removed `DisplayBuffer*` parameter
   and take a `FrontendSubmitSink` instead of `RingFeedSink`
   (`data_processor_batch_test`, `session_boundary_test`,
   `session_boundary_receiver_test`, `filesaver_round_boundary_test`,
   `count_boundary_save_binding_test`).
2. `paimage_host_output_test` now wires a real per-card `FrontendPreprocessor` for
   each tested card (production shape), with `HostOutput::setFrontendSessionSink`
   and `setFrontendBarrierSink` bound the way `NetworkController` binds them. Ring
   and DisplayBuffer assertions therefore observe the real delivery path. The
   display-preparation case moved to the stage, which is where that preparation
   now lives.
3. `paimage_host_output_test` multi-card case C3 previously asserted a single
   globally interleaved index sequence `0,0,1,1,2,2,3,3`. Task 3.2 mandates one
   worker per card, so two card workers interleave freely while each card's own
   trigger order is preserved. C3 now asserts the per-card logical index sequence
   `{0,1,2,3}` for each card plus the shared round generation, which is the
   property the case is actually about (multicard shared index). The later C7/C8
   expectations are order-independent and are unchanged.
4. `paimage_host_output_test` timeout case T12 gained one explicit happens-before
   (`until(ringCount == 2)`) before the trigger that raises the TimeoutBoundary.
   Its documented contract — the truncated round's two scans and the next round's
   three scans all reach Ring, with the partial round sealed into its own file —
   is preserved exactly and is now deterministic. The complementary behaviour
   (a frame still queued at the TimeoutBoundary is staleDropped) is covered by
   `frontend_preprocessor_test` T9 and is the behaviour task 3.6 mandates.
5. `data_processor_imaging_isolation_test` now measures the same isolation
   contract through the new pipeline: 4000 raw saves consumed while the downstream
   imaging queue is saturated with its worker delayed, plus a frontend submit
   exception that must not escape into the save/acquisition thread.

## 14. Hardware evidence language

This receipt covers source/code correctness and automated build/tests only.

```text
source/code correctness            = the changes in this task
automated tests/build/selftest     = 44/44 ctest PASS, mingw-debug build PASS
real hardware validation           = NOT PERFORMED in this task
hardware root-cause attribution    = NOT CLAIMED
```

Software PASS here is not hardware PASS. The project's standing state is
unchanged: A/B/C/D functional validation is `PASS_FOR_CURRENT_SCOPE`, and the
exact FPGA/LabVIEW source of the extra startup triggers remains `NOT_PROVEN`.
Nothing in this task asserts a protocol constant for 7/4007 or any other observed
field pattern.

## 15. Known limitations / not verified

- `processFrontendSignal()` is identity by design. No filter frequency response,
  order, cutoff or numeric output is implemented or verified.
- `FrontendSubmitResult::QueueBusy` is a reserved value and is not produced by the
  current bounded-critical-section `submit`.
- No performance measurement of the new stage was taken (performance tuning is
  out of scope for this task). The Snapshot exposes `maxDepth`, `currentDepth` and
  downstream counters so a later performance task can measure it without
  re-plumbing.
- No UI is added for the stage; `NetworkController::frontendSnapshot()` is the
  only observation entry point.
- No field/real-hardware run was performed against this SHA. A field test package
  would require a separate delivery staging pass per `BUILD_STANDARD`.
