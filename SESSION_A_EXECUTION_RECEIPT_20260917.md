# Session A Execution Receipt — 2026-09-17

## Scope

This receipt belongs to the review addendum
`TASKS/SessionA审查追加_修正诊断字段与执行回执_20260917-132651.md`.
The addendum changes only production diagnostic metadata and records independently
reproducible validation. The Session A normalizer state machine and its observable
round semantics were not changed.

## Git identity

```text
repository = https://github.com/ZiXuannnZhang/PAErealtimeImaging.git
implementation branch = codex/session-a-round-policy-core-20260917-114452
baseline branch = codex/physical-round-normalizer-integrated-20260916
starting SHA for addendum = 352ca9d67b1701e446e6519d30e811d685017922
implementation/fix commit = 803e0cf658a5bc907ea01091b9eeb22c582507f3
receipt commit = reported in the final execution report to avoid self-reference
local HEAD at code-validation point = 803e0cf658a5bc907ea01091b9eeb22c582507f3
local HEAD before final receipt push = reported out-of-band in the final execution report
remote branch HEAD after final receipt push = reported out-of-band in the final execution report
```

At the code-validation point, the tracked status was clean. The only untracked
path was the user-provided local task document, preserved without staging:

```text
git status --short --branch
## codex/session-a-round-policy-core-20260917-114452...origin/codex/session-a-round-policy-core-20260917-114452 [ahead 1]
?? CODEX_REPORTS/SessionA审查追加_修正诊断字段与执行回执_20260917-132651.md

git status --porcelain --untracked-files=no
(empty)
```

## Diagnostic correction

Before the fix, the `paimage.round` observer emitted these fixed values for every
event:

```text
firstVisibleFilterMode = true
acceptedControlInvisibleRisk = first-visible-scan-may-be-filtered
```

The observer now derives both values from the machine-readable event field:

```text
firstVisibleFilterMode = (event.startupFilterTriggerCount > 0)

startupFilterTriggerCount == 0:
  acceptedControlInvisibleRisk = none

startupFilterTriggerCount > 0:
  acceptedControlInvisibleRisk = startup-filter-may-consume-visible-scan
```

The existing `startupFilterTriggerCount` field remains authoritative. The periodic
source snapshot already used `roundStats.firstVisibleFilterMode`, so no additional
change was made there. No Normalizer classification or boundary behavior changed.

## Exact test commands and results

### Test-target build

Command executed from
`MC_410T_MultiCard/delivery`:

```powershell
& 'D:\Qt\Qt6.8.0\Tools\CMake_64\bin\cmake.exe' --build build/tests_all_mingw_debug --target physical_round_normalizer_test paimage_host_output_test ring_production_blockers_test ring_block_assembler_test ring_round_identity_test count_boundary_save_binding_test filesaver_round_boundary_test auto_save_round_coordinator_test -j 8
```

Result: exit 0; all requested targets were built or confirmed up to date.

### Direct regression execution

Command executed from `MC_410T_MultiCard/delivery` with Qt 6.8.0 and MinGW
13.1.0 prepended to `PATH`:

```powershell
$qtBin = 'D:\Qt\Qt6.8.0\6.8.0\mingw_64\bin'; $mingwBin = 'D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin'; $env:PATH = "$qtBin;$mingwBin;$env:PATH"; $testRoot = (Resolve-Path 'build/tests_all_mingw_debug').Path; $tests = @(@('physical_round_normalizer_test', (Join-Path $testRoot 'paimage_core/physical_round_normalizer_test.exe')), @('paimage_host_output_test', (Join-Path $testRoot 'paimage_host_output_test.exe')), @('ring_production_blockers_test', (Join-Path $testRoot 'ring_production_blockers_test.exe')), @('ring_block_assembler_test', (Join-Path $testRoot 'ring_block_assembler_test.exe')), @('ring_round_identity_test', (Join-Path $testRoot 'ring_round_identity_test.exe')), @('count_boundary_save_binding_test', (Join-Path $testRoot 'count_boundary_save_binding_test.exe')), @('filesaver_round_boundary_test', (Join-Path $testRoot 'filesaver_round_boundary_test.exe')), @('auto_save_round_coordinator_test', (Join-Path $testRoot 'auto_save_round_coordinator_test.exe'))); $failed = $false; foreach ($test in $tests) { Write-Output ("RUN {0}: & {1}" -f $test[0], $test[1]); & $test[1]; $exitCode = $LASTEXITCODE; Write-Output ("RESULT {0}: exit={1}" -f $test[0], $exitCode); if ($exitCode -ne 0) { $failed = $true } }; if ($failed) { exit 1 }
```

| Test | Result |
|---|---|
| `physical_round_normalizer_test` | PASS, exit 0; `T1-T18, A1-A12` |
| `paimage_host_output_test` | PASS, exit 0; production source/output/host boundary and timeout isolation regression |
| `ring_production_blockers_test` | PASS, exit 0; `failures=0` |
| `ring_block_assembler_test` | PASS, exit 0; `T1-T12,R1-R4,R10` |
| `ring_round_identity_test` | PASS, exit 0; `R5-R10,R14` |
| `count_boundary_save_binding_test` | PASS, exit 0; `C1-C10` |
| `filesaver_round_boundary_test` | PASS, exit 0 |
| `auto_save_round_coordinator_test` | PASS, exit 0 |

Diagnostic semantics were additionally verified by source review and production
compilation: the observer expression is exactly
`event.startupFilterTriggerCount > 0`, with `none` for X=0 and the configurable
startup-filter risk string for X>0.

## Windows build

Command executed from `MC_410T_MultiCard/delivery`:

```powershell
cmd /c build_mingw_debug.cmd
```

Result: exit 0. The script performed the `mingw-debug` configure/build using:

```text
Qt = D:\Qt\Qt6.8.0\6.8.0\mingw_64
MinGW = D:\Qt\Qt6.8.0\Tools\mingw1310_64 (13.1.0)
CMake = 3.30.5
Ninja = 1.12.1
code SHA = 803e0cf658a5bc907ea01091b9eeb22c582507f3
```

The required output check passed:

```text
PAimageReceiverDiagnostics.exe  present, 65246890 bytes
ImagingSvc.exe                  present, 3338451 bytes
ring_svc_selftest.exe           present, 1942332 bytes
ring_udp_replay.exe             present, 333547 bytes
```

CMake reported missing optional Vulkan headers, translation catalog, and
dxcompiler deployment files; these were non-fatal warnings and did not prevent
the required build outputs.

## Dependency receipt

This addendum did not change CUDA or Ring code. The build reused the existing
local runtime/dependency set:

```text
Ring import/runtime source = _migration_pack/prebuilt_cuda/bin
ring_recon_cuda.dll SHA256 = BF40472D5A46363A35DD1084A01A8C15F13EF203F3ED5B4BB5EDF767EEEC14B5
libring_recon_cuda.dll.a SHA256 = 94ABAB973582D06DEBF3831366BAEC00458EA3277F5F3B852CB1FA24A3C3F5
```

The ignored imaging runtime `cufft64_12.dll` was reused from the retained
Session A candidate worktree:

```text
source = _worktrees/physical-round-normalizer-integrated-20260916/MC_410T_MultiCard/delivery/libs/imaging/cufft64_12.dll
SHA256 = 2480D8AB849D7E9A375275F6C0278B8764C14AC0C1A3BDACAF256AE4A93C5590
```

## Scope and validation boundary

Changed production code is limited to:

```text
MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp
```

This addendum does not modify the Normalizer state machine, startup filtering,
CountBoundary/TimeoutBoundary semantics, RoundIdentity, Ring/CUDA, completion
protocol, or UI/QSettings behavior. The existing
`HANDOFF_SESSION_A_20260917.md` remains the Session B handoff; this receipt is
the addendum's independent execution evidence.

```text
SESSION_A_SOFTWARE_REVIEW_ADDENDUM = PASS
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS = NOT_PROVEN_BY_THIS_TASK
```
