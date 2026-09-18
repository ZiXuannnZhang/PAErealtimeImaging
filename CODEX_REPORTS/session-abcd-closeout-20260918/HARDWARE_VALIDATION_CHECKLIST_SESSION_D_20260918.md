# Hardware validation checklist — Session D candidate (2026-09-18)

This is an operational/evidence checklist for the next real
FPGA/NIC/LabVIEW window against the Session D candidate. It is preparation
only; **no hardware measurement was initiated in Session D** and every status
below is PENDING. Do not interpret any item here as a hardware PASS until a
live run produces the required evidence and it is reviewed.

## 1. Candidate/build/dependency identity (fill before any live run)

Every live run must record this identity first; field results whose running
binaries cannot be traced to this exact candidate cannot be used for
acceptance.

```text
candidate branch             = codex/session-d-integration-validation-20260918-000458
candidate code SHA            = d9daa2d7af6bb8341349e405433824e89e42bcd4
full delivery path            = D:/ChatGPT/PAERealtimeImaging/_worktrees/session-c/artifacts/build-delivery/20260918-005157_d9daa2d/
main exe (PAimageReceiverDiagnostics.exe) SHA256 = E7752E8D256EBFE7D91CA926E4473083FCB47C9CD9DBAAF721D56E568F6B2926
ring_recon_cuda.dll SHA256    = BF40472D5A46363A35DD1084A01A8C15F13EF203F3ED5B4BB5EDF767EEEC14B5
cudart64_12.dll SHA256        = C2C9A9C22A9BCBA90E261825968836787B331038047A26770CFFB7A583C28344
cufft64_12.dll SHA256         = 2480D8AB849D7E9A375275F6C0278B8764C14AC0C1A3BDACAF256AE4A93C5590
pa_recon_core.dll SHA256      = C1A37E73147C1B8250F96CCDC1FCFC08CA28CECBF1CB3C046D4AB426F2EA0893
build preset                  = mingw-debug (configure) + mingw-debug-build
toolchain                     = Qt 6.8.0 mingw_64, MinGW 13.1.0, Ninja 1.12.1, CMake 3.30.5
Ring CUDA source              = _migration_pack/prebuilt_cuda/bin (no local CUDA rebuild)
```

## 2. Per-run config snapshot to record

```text
startupFilterTriggerCount   (UI/QSettings RingConfigDialog/Defaults; default 1)
disableCountBoundary        (UI/QSettings; default false)
logicalTriggersPerRound
timeoutResetSec
enabled / connected control ports / devices
data ports per card
measurement session ID / run ID
ring radius per channel (if enabled)
sound speed settings (sos)
```

## 3. Scenario H1 — default compatibility

```text
startupFilterTriggerCount = 1
disableCountBoundary = false
```

Goal: prove the legacy-compatible configuration has no obvious regression.
Record: distinct physical count, filtered count, logical count, boundary
type/count, raw folder naming, Ring/CUDA diagnostics, next-round first logical
index. Result field: H1 = PENDING.

## 4. Scenario H2 — current multi-port startup hypothesis

```text
startupFilterTriggerCount = 7
disableCountBoundary = false
logicalTriggersPerRound = 4000
```

Note: 7 / 4007 is the current control-environment observation to verify, not a
protocol truth. Record the actuals regardless of expectation:
```text
physical distinct count
startup filtered count
logical accepted count
CountBoundary trigger/time
next-round first logical index
actual enabled/connected ports/devices
```

If the real count is not 4007, record it as-is and analyze; do not adjust the
conclusion to match 4007. Expected acceptance evidence (only if the hardware
really behaves so): 7 filtered startup distinct, 4000 logical distinct, one
CountBoundary aligned with the actual physical round end, next-round filter
restart. If the logs cannot prove FPGA/LabVIEW trigger semantics, write:
"operational positional filtering works for this run; exact FPGA/LabVIEW
trigger semantics remain unproven." Result field: H2 = PENDING.

## 5. Scenario H3 — variable short round (timeout closes)

```text
X = 7 (or the field-confirmed operational setting)
disableCountBoundary = true
N = 4000 reference cap
user stops the scan before 4000 logical triggers; idle > timeout
```

Verify and record: raw saved to the real stop position; realtime imaging to the
actual logical count; no CountBoundary; TimeoutBoundary exactly once;
generation++; old directory/raw remains old; timeout screenshot in
oldDirectory/recon_png (or manual recon dir in manual mode); Ring residual
discard/reset; next round index0/filter restart; the early/short round is not
claimed as reconstructionComplete. Result field: H3 = PENDING.

## 6. Scenario H4 — over-cap variable round

```text
disableCountBoundary = true
actual logical trigger count > 4000
```

Verify and record: raw continues past 4000; realtime Ring/CUDA stops after
logical index 3999; no modulo and no cap reopen inside the same physical round;
TimeoutBoundary closes the round; next round cap reopens from index 0. Result
field: H4 = PENDING.

## 7. Scenario H5 — partial startup timeout

```text
startupFilterTriggerCount = 7
only part of the startup triggers appear, then idle timeout
```

Verify and record: physical/filter lastCompleted counts are accurate; timeout
does not depend on entering CollectingScan; next round restarts the filter. May
be marked NOT RUN if it cannot be reproduced safely; this does not affect the
other scenarios. Result field: H5 = PENDING / NOT RUN.

## 8. Per-key-round observability to retain

For each key round, retain evidence so the chain can be reconstructed, not
just "the image looks normal":

```text
paimage round events (ControlFiltered / CountBoundary / TimeoutBoundary)
measurementSession / roundGeneration
physical / logical / filter counters
CountBoundary / TimeoutBoundary counts and timing
trigger gap / partial stats and missingTriggerCount
AutoSave boundary events / old-new directory naming
FileSaver rollover metadata (old/new generation, trigger count)
Ring transition residual/stale diagnostics; staleRoundDrops
ring_reset / service reset evidence; reset submit success/failure
submit_index stale cutoff / stale snapshot reject
next RoundIdentity clean start
PNG filenames / directories
raw file counts / sizes (per directory, per round generation)
```

When a chain link lacks log evidence, mark it UNVERIFIED / INCONCLUSIVE; do not
infer it from a normal-looking image.

## 9. Overall hardware status

```text
H1 = PENDING
H2 = PENDING
H3 = PENDING
H4 = PENDING
H5 = PENDING / NOT RUN
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS     = NOT PROVEN
```

Software 45/45 CTest, the Windows build PASS, the Ring/CUDA software selftest
PASS and the READY delivery candidate do not replace real hardware acceptance.
