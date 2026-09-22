# 前端高/低通零相位滤波落地与遗留口径清理：实现与验证回执

## 1. Task / source identity

Task document:

```text
origin/codex/task-docs:TASKS/前端高低通零相位滤波落地与遗留口径清理_20260923-010500.md
```

The remote copy was fetched with `git fetch --prune origin` and is byte-identical
to the untracked local `TASKS/` copy (`diff` = identical), so the specification
executed here is exactly the published one.

```text
repository                = ZiXuannnZhang/PAErealtimeImaging
branch                    = codex/frontend-filter-stage-20260923
STARTING_SHA              = 25804b4c2ab0de5c9bda6d91e8954970c7235bec
IMPLEMENTATION_SOURCE_SHA = 093f8cc8c3e1d8a5b75e8574d85520ec2b90e54d
REPORT_HEAD               = evidence-only receipt commit (see section 12)
```

Entry-state check (task section 2) passed before any edit:

```text
HEAD == 25804b4c2ab0de5c9bda6d91e8954970c7235bec   YES
git status --porcelain --untracked-files=no        empty
```

The branch was created at that exact SHA:

```text
git switch -c codex/frontend-filter-stage-20260923 25804b4c2ab0de5c9bda6d91e8954970c7235bec
```

No merge / rebase / cherry-pick of `main` or any other branch entered this task:

```text
git merge-base 25804b4c2ab0de5c9bda6d91e8954970c7235bec HEAD
  = 25804b4c2ab0de5c9bda6d91e8954970c7235bec
git rev-list --count 25804b4c..HEAD = 1
```

The starting SHA is the tip of `codex/frontend-preprocessor-stage-20260922`, not
`origin/main`, exactly as task section 2 specifies.  No force push, reset,
rebase or amend was used at any point.

Before implementation, the three governance documents were read from
`origin/main` (`PROJECT_STATUS.md`, `REPOSITORY_BASELINE.md`, `BUILD_STANDARD.md`)
and `BUILD_STANDARD.md` governed the build and evidence workflow below.

## 2. A 部分 — filter implementation as built

### 2.1 Structure, order range and edge policy

```text
filter family        Butterworth, high-pass and low-pass designed independently
representation      cascade of second-order sections (SOS), each stored as
                     [b0 b1 b2 a0 a1 a2] normalised to a0 = 1
section structure   direct II transposed, cascaded in section order
order range         N = 1 .. 8 (validated)
odd N               the cascade ends with one first-order section (b2 = a2 = 0)
execution           filtfilt: forward pass + backward pass per path -> zero phase
two-path order      high-pass first, then low-pass; the high-pass output is the
                     low-pass input.  Each path is its own complete filtfilt.
disabled path        skipped entirely: no coefficients designed, no arithmetic
edge policy         odd-symmetric reflection, L = 3 x N points per end
                     left  k-th point (k = 1..L, counted outward from x[0])
                           = 2*x[0] - x[k]      (buffer-leftmost is k = L)
                     right j-th point (j = 1..L, counted outward from x[n-1])
                           = 2*x[n-1] - x[n-1-j]
                     if n <= L then L = n - 1
                     if n < 2 the path returns the sequence unchanged
state policy        every filter pass starts from a zero initial state
                     (task 3.5 steps 2 and 3); no sample skipping, no fixed
                     sample offset, no system_delay truncation, no
                     position-dependent skip/trim rule of any kind
sample rate         FPGA_ADC_FREQ_HZ = 250e6, fixed, not a parameter.
                     SAMPLE_FREQ_HZ / SAMPLE_INTERVAL_NS / DIFF_SAMPLE_RATE_HZ /
                     FREQ_SCALE_KHZ take no part in any filter conversion
                     (verified by grep over the filter sources: the only hits are
                     a comment stating they are deliberately unused).
```

`N` second-order sections pair the conjugate poles `p_k` and `p_{N+1-k}` of the
analog prototype; section order is the increasing-`k` order of those pairs, with
the real-pole first-order section appended last when `N` is odd.

### 2.2 Design pipeline (task 3.3, as implemented)

```text
1. fs = FPGA_ADC_FREQ_HZ;  prewarped analog cutoff  W = 2*fs*tan(pi*fc/fs)
2. N-th order analog Butterworth prototype poles
      p_k = W * exp(j*pi*(2k + N - 1)/(2N)),  k = 1..N
   low-pass  : H_lp(s) = W^N / prod(s - p_k)          N zeros at infinity
   high-pass : H_hp(s) = s^N  / prod(s - q_k)         N zeros at s = 0
3. bilinear transform  s = 2*fs*(z-1)/(z+1)  -> H(z), per conjugate-pole pair
   (and per real pole for odd N)
4. gain normalisation: low-pass unit gain at DC (z = 1),
                       high-pass unit gain at Nyquist (z = -1)
5. each pair becomes one [b0 b1 b2 a0 a1 a2] section with a0 = 1
6. sections cascaded in order, direct II transposed inside each section
```

**Assumption stated for step 2 (high-pass spectral transform).**  Task 3.3 says
`p_k = W * exp(...)` (a prototype already scaled to cutoff `W`) and then says the
high-pass comes from `s -> W/s`.  Taken together those are dimensionally
inconsistent: `W/s` is dimensionless while a pole must carry units of `s`.  The
reading implemented here is the one that satisfies every other hard requirement
in 3.3/3.4 — an N-th order Butterworth high-pass whose `-3 dB` cutoff is exactly
`fc` (the prewarped `W`), with N zeros at `s = 0` and unit gain at Nyquist:

```text
s -> W^2 / s   applied to the W-scaled prototype
  == s -> W / s  applied to the unit-cutoff prototype
```

Under this reading the high-pass poles are `q_k = conj(p_k)` (the mirror image of
the low-pass poles across the imaginary axis), which is the standard Butterworth
low-pass to high-pass transform.  Measured confirmation is in section 3.1.

### 2.3 Parameters as implemented

| parameter | type | range | default | notes |
|---|---|---|---|---|
| 启用高通 | bool | — | `true` | independent enable |
| 高通截止(MHz) | double | (0, 125) MHz, open | `0.4` | UI unit MHz, design in Hz |
| 高通阶数 | int | 1 .. 8 | `2` | |
| 启用低通 | bool | — | `true` | independent enable |
| 低通截止(MHz) | double | (0, 125) MHz, open | `60.0` | UI unit MHz, design in Hz |
| 低通阶数 | int | 1 .. 8 | `2` | |

Sample rate is fixed at 250 MHz and is not a parameter.

Validation runs before every delivery.  Any violation rejects the whole delivery,
leaves the effective configuration untouched, and shows the reason in the UI:

```text
cutoff must be > 0 and < 125 MHz
order  must be within 1 .. 8
when both paths are enabled, low-pass cutoff must be strictly greater than
  the high-pass cutoff
```

The `QDoubleSpinBox` range note: Qt spin boxes express closed intervals only, so
the two cutoff controls use `[0.001, 124.999]` — the open interval `(0, 125)`
realised at the control's 3-decimal resolution.  `frontend_filter::validate` is
the authority (`> 0` and `< 125`) and rejects any out-of-range value regardless
of how it was produced.

### 2.4 Single insertion point

Filtering is implemented in exactly one place: `FrontendPreprocessor::
processFrontendSignal()`.  A repository-wide grep for `filtfilt` / `filterForward`
/ `designLowpass` / `designHighpass` / `frontend_filter::Bank` finds call sites
only in `FrontendPreprocessor.cpp` (the insertion point), `FrontendFilter.*` (the
implementation) and `tests/frontend_preprocessor_test.cpp` (the contract tests).
There is no second filter or filter copy in DataProcessor, HostOutput,
FrameConverter, DisplayBuffer, MainWindow, ImagingBypass, RingBlockAssembler,
FileSaver or FramePublisher.

```text
FrontendPreprocessor worker (one instance per card)
  dequeue raw const
  -> freeze this frame's coefficients        <- added for 3.6 effective moment
  -> stale check
  -> deep copy -> frontend clone
  -> processFrontendSignal(frontend clone)   <- THE filter insertion point
  -> full-resolution display preparation
  -> stale check again
  -> dispatch
       |- DisplayBuffer::update / updateFullRes
       `- RingFeedSink / ImagingBypass
```

- filtering rewrites `freqA` / `freqB` of the frontend clone **in place**;
- the raw `TriggerGroup` passed to `submit()` is never mutated (save and
  FramePublisher keep consuming it) — asserted point by point;
- processing order stays **filter -> display derivation -> dispatch**;
- `prepareDisplayData` and the `phase*_display` derivation were **not** modified
  (task 3.1 last bullet and section 6); it continues to read the filtered
  `freqA` / `freqB` in its existing order, and this task does nothing extra to it.

### 2.5 Per-A-line independence

`freqA` and `freqB` use the same coefficient set and are filtered independently
over the complete sample sequence of that channel within the TriggerGroup.  The
`Bank` is immutable after construction and carries no state: no filter state,
history sample or concatenated sample is retained or passed between A-lines,
triggers or cards.  Every A-line starts from a zero filter state.  16-bit and
32-bit payloads and any `sampleCount` are handled (length is taken from each
channel's own vector).

### 2.6 Config delivery path and effective-moment implementation point

```text
RingConfigDialog::applyConfig()
  -> validate (3.4)  -- on failure: nothing is delivered at all, UI shows why
  -> emit frontendFilterChanged(config)
       -> MainWindow (connect in ensureRingConfigDialog)
            -> NetworkController::setFrontendFilterConfig(config)
                 -> FrontendPreprocessor::setFilterConfig(config)  [one per card]
                      -> design the new Bank in full, off-lock
                      -> swap the shared_ptr<const Bank> under filterMutex_
```

This path is **independent of the reconstruction configuration delivery**
(`ImagingController::configureRing` / `RingReconCudaConfig`).  Filter parameters
are Frontend Preprocessing configuration only; they are never written into
`RingReconCudaConfig`, ImagingSvc, ring_recon configuration or any reconstruction
JSON field (verified by grep over those trees: zero hits).

**Effective moment** — `FrontendPreprocessor::workerLoop()` freezes
`filterBank_` into a local `shared_ptr` immediately after it dequeues a frame:

```text
MC_410T_MultiCard/delivery/src/FrontendPreprocessor.cpp
  workerLoop(): "item = std::move(queue_.front())"  then  "frameBank = filterBank_"
  ... later ...
  processFrontendSignal(*frontend, *frameBank)
```

so a frame already taken by the worker completes on the coefficients it started
with, and every frame dequeued after the update returns uses the new ones.  This
satisfies both halves of task 3.6 — "已经在处理中的帧用旧系数完成" and
"其后出队的下一帧开始使用新系数" — and involves no round boundary, no
measurement-session boundary and no service restart.

Coefficient caching and thread safety (task 3.6):

```text
SOS designed once per configuration change, never per frame
immutable Bank + shared_ptr exchange under a dedicated filterMutex_
  - the worker holds that lock only for a pointer copy (never across filtering)
  - the UI thread only swaps a fully designed Bank in (never waits on the worker)
one FrontendPreprocessor per card, each with its own coefficient cache and its
  own copy of the configuration (NetworkController fans the parameters out and
  each stage designs its own Bank)
```

Startup delivery: `MainWindow::startListeningWithIPs()` applies the persisted
parameters via `NetworkController::setFrontendFilterConfig(RingConfigDialog::
loadFrontendFilterDefaults())` before `start()`, so a newly created stage inherits
them at construction.  This mirrors the existing Session B round-policy startup
delivery and is what makes the persistence actually take effect; without it the
filter would fall back to factory defaults until the dialog was opened.

### 2.7 Parameter UI and persistence

Location: `RingConfigDialog`, 「重建参数」 tab, new group box titled
「前端滤波（逐 A-line 零相位）」, inserted immediately **before** the existing
「预处理」 group box (order in the tab: 成像网格与 DAS, 声速模型,
前端滤波（逐 A-line 零相位）, 预处理).

Six controls, with the per-path enable/disable linkage (unchecked path's cutoff
and order controls become disabled, their values kept and not cleared):

```text
启用高通        QCheckBox       default checked
高通截止(MHz)   QDoubleSpinBox  default 0.4   range (0,125) open, 3 decimals
高通阶数        QSpinBox        default 2     range 1..8
启用低通        QCheckBox       default checked
低通截止(MHz)   QDoubleSpinBox  default 60    range (0,125) open, 3 decimals
低通阶数        QSpinBox        default 2     range 1..8
```

`应用` / `确定` reuse the existing `applyConfig()` flow: after the 3.4 validation
passes, the parameters are delivered to the Frontend Preprocessing configuration
through the MainWindow -> NetworkController wiring, independent of the
reconstruction delivery; on validation failure the **whole** apply is refused and
the UI shows the reason.

Persistence follows the dialog's existing `saveDefaults()` / `restoreDefaults()`
mechanism, same `QSettings(paimageSettingsPath(), QSettings::IniFormat)`, same
`RingConfigDialog/Defaults` group, key naming style consistent with the group's
existing keys (`feHpEnable` … camelCase).  The six keys are read through the same
`val(...)` lookup and written through the same `s.setValue(...)` calls as the
neighbouring keys:

```text
feHpEnable     bool     default true
feHpCutoffMhz  double   default 0.4
feHpOrder      int      default 2
feLpEnable     bool     default true
feLpCutoffMhz  double   default 60.0
feLpOrder      int      default 2
```

Missing historical keys fall back to the table above.  No pre-existing key in that
group had its meaning or default changed.  「设为默认」 / 「恢复默认」 use the same
read/write path for these six keys as for the existing keys.
`RingConfigDialog::loadFrontendFilterDefaults()` reuses the same key constants for
the pre-dialog startup read, so the three sites cannot drift apart.

## 3. Filter numerics verification

### 3.1 Measured on the implemented designer (not asserted from theory)

```text
N = 1..8  low-pass : |H(1)| = 1.000000000000   |H(-1)| = 0
N = 1..8  high-pass: |H(-1)| = 1.000000000000   |H(1)| = 0
section count = ceil(N/2) for both paths; odd N last section has b2 = a2 = 0

cutoff check (N = 2), |H(fc)| should be 0.707107:
  fc =   0.4 MHz   low-pass 0.707107   high-pass 0.707107
  fc =  10.0 MHz   low-pass 0.707107   high-pass 0.707107
  fc =  60.0 MHz   low-pass 0.707107   high-pass 0.707107
  fc = 120.0 MHz   low-pass 0.707107   high-pass 0.707107
```

So the `-3 dB` point lands exactly on `fc` for both paths across the whole
allowed range — the concrete confirmation of the spectral-transform reading in
section 2.2.

### 3.2 Zero-phase measurement used to calibrate contract check 3

Relative asymmetry `max_i |y[i] - y[n-1-i]| / max|y|`:

```text
compact symmetric bump centred in a zero-margin buffer, n = 4096,
config = high-pass 20 MHz order 2 + low-pass 60 MHz order 2
  filtfilt (forward + backward)   2.334e-07     <- float32 rounding level
  single forward pass             1.1551e-02    <- 50000x worse
```

## 4. A 部分 — snapshot observability

Added to `FrontendPreprocessor::Snapshot`:

```text
filteredFrames        frames on which a filter pass actually ran
filterDesigns         SOS coefficient re-design count
filterConfigVersion   version of the currently effective configuration
```

Semantics as implemented:

```text
filteredFrames       +1 per frame where at least one enabled path actually
                     filtered (frames with both paths disabled, and frames whose
                     sequence has n < 2 and therefore return unchanged, are not
                     counted)
filterDesigns        +1 per accepted configuration change; identical parameters
                     re-delivered are not a change and do not re-design
filterConfigVersion  +1 alongside filterDesigns; 0 = the configuration the stage
                     was constructed with (task 3.4 factory defaults)
```

Isolation matches the existing fields exactly: these counters are never folded
into UDP packet loss, `missingTriggerCount`, `saveQueueDiscards` or any imaging
discard statistic.  The existing `NetworkController::frontendSnapshot(cardIdx)`
per-card query returns them without change to its interface — which is why no
code outside `FrontendPreprocessor` had to be touched for observability.  No new
UI display was added (task 4).

## 5. B 部分 — legacy cleanup results (all four items executed)

| # | item | premise check | result |
|---|---|---|---|
| 1 | `MainWindow.cpp` 「唯一裁切节点：presentation（后续接入零相位滤波）」 comment | found at the `onDisplayRefresh` crop block | **DONE** — comment rewritten to point at the FrontendPreprocessing filter insertion point `FrontendPreprocessor::processFrontendSignal()`, stating the filtering completes there and the UI side only does viewport crop and rendering.  Comment text only; no code line changed. |
| 2 | `MainWindow.cpp` two `fmtDesc` sampling-rate strings | found in `loadSettings()` | **DONE** — the rate and interval are now both computed from the link's single sampling-rate source `m_sampleIntervalNs` and displayed as actual values (`%1MSa/s %2（%3ns/点）` with `%1 = 1000/m_sampleIntervalNs`, `%3 = m_sampleIntervalNs`).  The 「125MSa/s」 and 「2抽1」wording is gone; bit-width distinction keeps only the `Q16.16` / `Q0.15` labels.  String content only. |
| 3 | `Constants.h` `SAMPLE_FREQ_HZ` and `SAMPLE_INTERVAL_NS` | whole-repo reference check | **DONE** — deleted.  Premise held: zero reference points.  Verified with a whole-repo grep for both identifiers; the only hits were the two declarations themselves, the task document (untracked, on `codex/task-docs`) and one hit each inside the `ogprog/` historical tree, which is **untracked** (`git ls-files ogprog` = 0 files) and is not part of the built source.  Because there were no reference points, no legacy annotation was required at the declaration site; a short comment records the deletion and the reason. |
| 4 | `MainWindow.cpp` / `MainWindow.h` `calculateFrequency` | call-site check | **DONE** — deleted (declaration + definition + its comment banner).  Premise held: zero call points.  Whole-repo grep found only the declaration, the definition and the banner comment in the live tree (plus its own separate copy in the untracked `ogprog/` historical tree).  No references remained, so nothing is listed. |

Item 3 note: `DIFF_SAMPLE_RATE_HZ` and `FREQ_SCALE_KHZ` were deliberately left in
place — the task scoped item 3 to `SAMPLE_FREQ_HZ` / `SAMPLE_INTERVAL_NS` only, and
both remaining constants already carry a comment marking them as historical.

## 6. Tests

New filter tests are merged into the existing `frontend_preprocessor_test` target
(one of the two options the task allows).  Reason: that target already builds the
stage with `FRONTEND_PREPROCESSOR_TEST_SEAM`, already has the deterministic
`WorkerGate` for in-flight/queued control, and already asserts the
one-clone/two-consumers dispatch contract that check 8 asks to reuse.  Adding a
separate target would have duplicated all of that scaffolding.

`F1..F13` map 1:1 onto the task's required checks 1..13:

| check | test | result | what it asserts |
|---|---|---|---|
| 1 | `testFilterDisabledIsIdentity` | PASS | both paths disabled -> output equals input point by point, `apply` reports no pass, `filteredFrames == 0` |
| 2 | `testFilterPathsExecute` | PASS | high-pass only / low-pass only / both: all three execute, output length == input length, output finite and actually altered |
| 3 | `testFilterZeroPhase` | PASS | symmetric input -> output symmetric about the sequence centre (2.334e-07), contrasted against a single forward pass (1.1551e-02) to prove the assertion is discriminating |
| 4 | `testFilterPerALineIndependence` | PASS | see the reading note below; (a) no state carry-over and order independence, (b) two segments joined in one buffer match the isolated runs to float rounding, (c) merging two A-lines into one call measurably differs (0.654) — the direct proof no concatenation happens |
| 5 | `testFilterEdgeExtension` | PASS | `L = 3 x order`, odd-reflection values asserted point by point against both a re-computed rule and a hand-computed sequence `[-5..0, 1..20, 21..26]`, `L` clamped to `n-1`, output trimmed back to `n` |
| 6 | `testFilterShortSequence` | PASS | `n = 0..8` across orders 1..8: deterministic, no crash, output length == input length, `n < 2` returned unchanged, otherwise finite |
| 7 | `testFilterSaveIsolation` | PASS | raw `freqA`/`freqB` pointwise unchanged before/after, raw save payload untouched, only the frontend clone rewritten |
| 8 | `testFilterSharedCloneDispatch` | PASS | DisplayBuffer and Ring sink get the same frontend clone (existing one-clone/two-consumers assertion style) and that clone carries the filtered sequence |
| 9 | `testFilterEffectiveNextFrame` | PASS | in-flight frame completes on its frozen coefficients, the next dequeued frame uses the new ones, neither is interrupted |
| 10 | `testFilterDesignCounters` | PASS | 5 frames under one config -> no growth; one change -> `filterDesigns` and `filterConfigVersion` each +1 in step; identical re-delivery -> no growth; a rejected config -> no growth |
| 11 | `testFilterConcurrentConfigUpdate` | PASS | 3 x 150 frames with a UI thread flipping two configs concurrently: no exception, lengths preserved, all outputs finite (a torn coefficient read would produce NaN/Inf), repeatable |
| 12 | `testFilterConfigValidation` | PASS | cutoff out of range, order out of range, and `lp <= hp` with both enabled each reject and leave the effective config, `filterDesigns` and `filterConfigVersion` untouched |
| 13 | `testFilterExceptionContainment` | PASS | a fault thrown inside the filter is counted in `exceptionDropped`, never escapes the worker, the raw save thread is unaffected and later frames keep flowing |

**Reading note for check 4.**  The wording "两段序列分别滤波的结果与拼接后整体滤波
得到的对应片段一致，差异在浮点舍入量级" is achievable at float-rounding level only
under the reading "the two segments are held in one joined buffer and processed
together"; that is what (a)+(b) assert.  The other reading — one `filtfilt` call
over the concatenated samples treated as a **single** A-line — cannot agree at
float-rounding level for an IIR filtfilt: that operation has edge padding only at
the two outer ends, whereas per-A-line filtering pads every A-line end, and the
junction behaves differently.  That behaviour is precisely what task 3.2 forbids
("不得在 A-line 之间…拼接样本"), so it is asserted as a **difference** in (c)
(measured deviation 0.654 against a peak-normalised signal), which turns the
ambiguity into a direct proof that A-lines are never merged.

**Reading note for check 3.**  With the zero initial state mandated by task 3.5
steps 2 and 3, a `filtfilt` cannot be exactly symmetric for an input that is
non-zero at the padded ends: the forward pass cold-starts at one end of the buffer
and the backward pass cold-starts at the other.  Check 3 therefore uses a
compactly supported symmetric bump centred in a long buffer that is exactly zero
at both ends (so neither pass has anything to ring from) and asserts symmetry at
float-rounding level (2.334e-07 measured against a 1e-6 threshold).  The
single-forward-pass contrast (1.1551e-02) shows the assertion distinguishes
zero-phase from a phase-shifted filter by ~5 orders of magnitude.

Existing tests updated because the interface/behaviour change affected them:

```text
frontend_preprocessor_test            T1 (identity deep-copy) and T2
                                      (full-resolution) now disable both filter
                                      paths explicitly: they assert architecture
                                      contracts on an identity frontend and the
                                      filter numerics belong to F1..F13.  Their
                                      assertions are otherwise unchanged.
data_processor_imaging_isolation_test now configures the default filter
                                      explicitly and asserts the raw save payload
                                      is never filter-rewritten, which extends its
                                      save-isolation contract to the new filter.
                                      Its 4000-frame pressure assertions are
                                      unchanged.
paimage_host_output_test              makeFrontend() disables both filter paths.
                                      Its golden assertion is a payload-routing
                                      fingerprint (per-card constant amplitude on
                                      the Ring path) that detects cross-card
                                      mix-up; the now-active filter rewrites it.
                                      Filter numerics are covered by F1..F13.
paimage_network_test                  same root cause, same fix, applied through
                                      the production
                                      NetworkController::setFrontendFilterConfig()
                                      before start().  Caught by the full run.
```

## 7. Required regression (task section 8)

All twelve targeted tests, each result:

```text
frontend_preprocessor_test              PASS
data_processor_imaging_isolation_test   PASS
data_processor_batch_test               PASS
imaging_bypass_test                     PASS
ring_block_assembler_test               PASS
ring_round_identity_test                PASS
paimage_host_output_test                PASS
session_boundary_test                   PASS
session_boundary_receiver_test          PASS
filesaver_round_boundary_test           PASS
count_boundary_save_binding_test        PASS
timeout_presentation_test               PASS

12/12 PASS, 0 FAIL, 0 SKIP, 0 NOT_RUN   (12.23 s)
```

Full `ctest -N` then `ctest --output-on-failure -j 4`:

```text
total   = 44
PASS    = 44
FAIL    = 0
SKIP    = 0
NOT_RUN = 0
```

Failure history, each with one necessary diagnosis and no rerun loops (full
detail in `ctest-targeted.txt` / `ctest-full.txt`):

```text
targeted pass 1 : 10/12 — data_processor_imaging_isolation_test (my own new
                  assertion miscounted the capturing sinks: 4001 vs 4000) and
                  paimage_host_output_test (filter now active on the frontend
                  path).  Both fixed; targeted pass 2 = 12/12.
full pass 1     : 43/44 — paimage_network_test, same filter root cause.  Fixed;
                  full pass 2 = 44/44.
```

No unrelated module was modified by any of these fixes.

## 8. Build result and BuildIdentity

```text
BUILD_COMMAND   = cmd /c ".\build_mingw_debug.cmd"   (configure + build)
result          = PASS, exit 0
configure       = mingw-debug (Configuring done / Generating done)
build preset    = mingw-debug-build, CMake 3.30.5, Ninja 1.12.1
compiler        = GNU 13.1.0 (MinGW 13.1.0 / Qt Tools mingw1310_64)
Qt              = Qt 6.8.0 mingw_64
windeployqt     = executed (Qt runtime + plugins deployed; no bare exe delivered)
required exes   = PAimageReceiverDiagnostics.exe, ImagingSvc.exe,
                  ring_svc_selftest.exe, ring_udp_replay.exe  — all PRESENT
```

BuildIdentity (`build/mingw_debug/generated/PaimageAcquisition/BuildIdentity.h`)
recorded on the final production/test source commit with a tracked-clean tree:

```text
PAIMAGE_GIT_SHA=093f8cc8c3e1d8a5b75e8574d85520ec2b90e54d
PAIMAGE_TRACKED_DIRTY=false
PAIMAGE_BUILD_TYPE=Debug
PAIMAGE_COMPILER=GNU 13.1.0
PAIMAGE_GIT_SHA == IMPLEMENTATION_SOURCE_SHA : YES
```

(`PAIMAGE_PRODUCT_BASELINE` and `PAIMAGE_SOURCE_SHA256` are fixed literals in
`BuildIdentity.h.in`, not commit-derived, and therefore repeat the previous
receipt's values; only `PAIMAGE_GIT_SHA` / `PAIMAGE_TRACKED_DIRTY` track the
commit.)

Dependency provenance and SHA256 (unchanged by this task — no CUDA, imaging or
dependency change):

```text
Ring CUDA source = local build (build/ring_recon_cuda); _migration_pack fallback NOT used
ZeroMQ source    = repository third_party/zeromq (v4.3.5)

c1a37e73147c1b8250f96ccdc1fcfc08ca28cecbf1cb3c046d4ab426f2ea0893  libs/imaging/pa_recon_core.dll
2480d8ab849d7e9a375275f6c0278b8764c14ac0c1a3bdacaf256ae4a93c5590  libs/imaging/cufft64_12.dll
bf40472d5a46363a35dd1084a01a8c15f13ef203f3ed5b4bb5edf767eeec14b5  build/ring_recon_cuda/bin/ring_recon_cuda.dll
c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344  build/ring_recon_cuda/bin/cudart64_12.dll
37610023d91951bc4177db1f96b54b28911976ac70b221a852887709a02a9ad3  third_party/zeromq/bin/libzmq-v141-mt-4_3_5.dll
```

## 9. Prohibited scope (task section 6) — verified on the tree

```text
MATLAB reference parameters / defaults / thresholds / orders / cutoffs /
  sample-position rules (skip-start, system_delay, DelayCut, mask length, crop)  NONE
phase*_display derivation formula or implementation                            UNCHANGED
save file format, FileSaver, FramePublisher, raw save content,
  session/round binding                                                        UNCHANGED
filtering or sample rewriting on save / publish / UDP ingress / FrameConverter  NONE
filter parameters in RingReconCudaConfig / ImagingSvc / ring_recon / its JSON   NONE
ring reconstruction algorithm, RingReconCudaConfig layout,
  CUDA ABI, any DLL interface                                                  UNCHANGED
display decimation / downsampling                                              NOT reintroduced
linear-imaging legacy filter (ImagingSvc filterType) and its configuration      UNCHANGED
PhysicalRoundNormalizer, RoundIdentity, stale barrier,
  CountBoundary / TimeoutBoundary semantics                                    UNCHANGED
TriggerGroup field meanings, decodeRaw, computeFrequency                       UNCHANGED
performance optimisation, GPU work, parallelisation, unrelated refactoring     NONE
third-party numeric library or new external dependency                         NONE
force push / reset / rebase / amend / pushed-history rewrite                   NONE
```

## 10. Changed production / test files

```text
production / build input (11):
  MC_410T_MultiCard/delivery/CMakeLists.txt
  MC_410T_MultiCard/delivery/include/Constants.h
  MC_410T_MultiCard/delivery/include/FrontendFilter.h                (new)
  MC_410T_MultiCard/delivery/include/FrontendPreprocessor.h
  MC_410T_MultiCard/delivery/include/MainWindow.h
  MC_410T_MultiCard/delivery/include/NetworkController.h
  MC_410T_MultiCard/delivery/include/RingConfigDialog.h
  MC_410T_MultiCard/delivery/src/FrontendFilter.cpp                  (new)
  MC_410T_MultiCard/delivery/src/FrontendPreprocessor.cpp
  MC_410T_MultiCard/delivery/src/MainWindow.cpp
  MC_410T_MultiCard/delivery/src/NetworkController.cpp
  MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp
  MC_410T_MultiCard/delivery/src/RingConfigDialog.cpp

tests / test build input (5):
  MC_410T_MultiCard/delivery/tests/CMakeLists.txt
  MC_410T_MultiCard/delivery/tests/data_processor_imaging_isolation_test.cpp
  MC_410T_MultiCard/delivery/tests/frontend_preprocessor_test.cpp
  MC_410T_MultiCard/delivery/tests/paimage_host_output_test.cpp
  MC_410T_MultiCard/delivery/tests/paimage_network_test.cpp

18 files, +1556 / -63 against STARTING_SHA
```

## 11. Evidence language

Everything above is **source/code correctness plus automated build and test
evidence**.  This task claims no real-hardware validation and no hardware
root-cause attribution.  Filter numerics were verified against the task's own
design definition (Butterworth magnitude, unit gain at DC/Nyquist, `-3 dB` at
`fc`, SOS cascade, filtfilt zero phase) and were deliberately **not** compared
against `实时重建脚本/` or `RadiusCalibration/` MATLAB scripts — importing
parameters or rules from those was prohibited by task section 6.

Known limitations / not verified:

```text
real hardware behaviour of the filter chain       NOT RUN (out of task scope)
comparison against MATLAB reference outputs       NOT DONE (prohibited scope)
ThreadSanitizer / ASan run of the concurrency case NOT RUN (stress case only)
```

## 12. Identity and push

```text
Implementation branch     codex/frontend-filter-stage-20260923
Starting SHA              25804b4c2ab0de5c9bda6d91e8954970c7235bec
IMPLEMENTATION_SOURCE_SHA 093f8cc8c3e1d8a5b75e8574d85520ec2b90e54d
Evidence directory        CODEX_REPORTS/frontend-filter-stage-20260923/
```

Receipt-only diff contract: `REPORT_HEAD` against `IMPLEMENTATION_SOURCE_SHA`
contains **only** `CODEX_REPORTS/frontend-filter-stage-20260923/`
(`validation-receipt.md`, `build-summary.txt`, `ctest-targeted.txt`,
`ctest-full.txt`, `git-receipt.txt` — all text).  No build tree, executable, DLL,
archive or other binary is committed.  Verified after the receipt commit with
`git diff --name-only IMPLEMENTATION_SOURCE_SHA..REPORT_HEAD`.

Because a commit cannot embed its own SHA, `REPORT_HEAD` is reported in the final
task report together with the three-way push verification of task section 10
(`git rev-parse HEAD`, `git rev-parse origin/codex/frontend-filter-stage-20260923`,
`git ls-remote origin refs/heads/codex/frontend-filter-stage-20260923`).
