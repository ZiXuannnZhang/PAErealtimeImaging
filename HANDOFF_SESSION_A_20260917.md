# Session A Handoff — PhysicalRoundNormalizer core policy

## Repository state

- Repository: `ZiXuannnZhang/PAErealtimeImaging`
- Implementation branch: `codex/session-a-round-policy-core-20260917-114452`
- Baseline branch: `codex/physical-round-normalizer-integrated-20260916`
- Exact starting SHA: `52cf7713d7e0e935cb14663ec3470f3a25bfeb90`
- Implementation commit SHA: reported in the execution receipt; intentionally not embedded here to avoid a self-referential report commit.
- Final remote HEAD: reported in the execution receipt after push.
- Handoff generation status: implementation files and this handoff are the only tracked changes; ignored build output and the local CUDA runtime are not part of Git status.

## Implemented scope

- Added configurable `startupFilterTriggerCount`, default `1`.
- Added `disableCountBoundary`, default `false`.
- Added current and last-completed per-round physical/startup-filter counters.
- Preserved cumulative physical, filtered, logical, CountBoundary, and TimeoutBoundary counters.
- Added `PhysicalRoundEvent`/`Snapshot` metadata for the new policy and counters.
- Exposed the policy through `HostOutput`, `Backend::Settings`, and `NetworkController` production seams.
- Kept Ring, ImagingSvc, FileSaver, CUDA, completion protocol, and UI/QSettings behavior outside Session A.

## Exact semantics

- `startupFilterTriggerCount = 0`: first new distinct physical identity is `LogicalScan` index `0`.
- `startupFilterTriggerCount = 1`: first new distinct identity is filtered; the second is logical index `0`.
- `startupFilterTriggerCount = N`: the first `N` new distinct identities are filtered; the next identity is logical index `0`.
- Repeated cards/cache hits reuse the original decision and do not increment current physical/filtered/logical counters or consume a startup slot.
- With `disableCountBoundary = false`, configured logical count retains the one-shot `roundComplete`/stable `isFinalLogicalTrigger` behavior, latches per-round counts, clears current counts, and advances `roundGeneration`.
- With `disableCountBoundary = true`, configured logical count does not create a boundary or final marker; logical indices continue increasing in the same `RoundIdentity` until timeout.
- `timeoutBoundary()` is effective for either CountBoundary mode and can close a partial startup-filter round after at least one new distinct identity.
- Timeout latches current physical/filtered counts, clears current physical/filtered/logical counts, advances `roundGeneration`, and starts the next round with the configured startup policy.
- `beginSession()` clears cumulative counters, current/last-completed per-round counters, cache, and round state; startup filtering restarts.

## Production interfaces for next sessions

Normalizer APIs:

```text
void setStartupFilterTriggerCount(std::uint64_t count)
std::uint64_t startupFilterTriggerCount() const
void setDisableCountBoundary(bool disable)
bool disableCountBoundary() const
```

Host/controller forwarding APIs:

```text
HostOutput::setStartupFilterTriggerCount(std::uint64_t)
HostOutput::setDisableCountBoundary(bool)
NetworkController::setStartupFilterTriggerCount(std::uint64_t)
NetworkController::setDisableCountBoundary(bool)
```

`Backend::Settings` carries `startupFilterTriggerCount` and `disableCountBoundary` into `HostOutput`.

`PhysicalRoundNormalizer::Snapshot` fields for Session B/C:

```text
startupFilterTriggerCount
disableCountBoundary
currentPhysicalDistinctCount
currentStartupFilteredCount
lastCompletedPhysicalDistinctCount
lastCompletedStartupFilteredCount
```

The same fields are present on `PhysicalRoundEvent` for boundary diagnostics. Existing cumulative fields and `currentLogicalDistinctCount` remain available.

## Modified files

- `MC_410T_MultiCard/delivery/include/PaimageAcquisition/PhysicalRoundNormalizer.h`
- `MC_410T_MultiCard/delivery/src/PaimageAcquisition/PhysicalRoundNormalizer.cpp`
- `MC_410T_MultiCard/delivery/include/PaimageAcquisition/HostOutput.h`
- `MC_410T_MultiCard/delivery/src/PaimageAcquisition/HostOutput.cpp`
- `MC_410T_MultiCard/delivery/include/PaimageAcquisition/Backend.h`
- `MC_410T_MultiCard/delivery/src/PaimageAcquisition/Backend.cpp`
- `MC_410T_MultiCard/delivery/include/NetworkController.h`
- `MC_410T_MultiCard/delivery/src/NetworkController.cpp`
- `MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp`
- `MC_410T_MultiCard/delivery/tests/paimage_core/physical_round_normalizer_test.cpp`
- `MC_410T_MultiCard/delivery/tests/paimage_host_output_test.cpp`
- `HANDOFF_SESSION_A_20260917.md`

## Frozen invariants

- A shared decision is created once per `(measurementSession, triggerSeq)` and reused by late cards while cached.
- `roundComplete` remains one-shot; `isFinalLogicalTrigger` remains a stable data-plane property.
- `RoundIdentity = (measurementSession, roundGeneration)` remains the cross-stage ownership key.
- Physical idle timeout remains distinct from SourceCore single-card packet-assembly timeout.
- The recent decision cache remains bounded.
- `sourceRoundComplete`, `reconstructionComplete`, `expectedBlocks`, final PNG rules, AutoSaveRoundCoordinator, FileSaver, Ring, ImagingSvc, and CUDA behavior are not redefined here.

## Tests / build

- `physical_round_normalizer_test` — PASS; existing T1–T18 plus Session A A1–A12.
- `paimage_host_output_test` — PASS; production HostOutput path plus Session A setter/snapshot seam.
- `ring_production_blockers_test` — PASS (`failures=0`).
- `ring_block_assembler_test` — PASS.
- `ring_round_identity_test` — PASS.
- `count_boundary_save_binding_test` — PASS (`C1-C10 ALL PASS`).
- `filesaver_round_boundary_test` — PASS.
- `auto_save_round_coordinator_test` — PASS.
- Standard Windows build — PASS with `cmd /c build_mingw_debug.cmd` after configure; required executables were present.

Test/build environment:

```text
Qt 6.8.0 mingw_64
MinGW Qt Tools/mingw1310_64
CMake Qt Tools/CMake_64
Ninja Qt Tools/Ninja
CMake preset = mingw-debug
```

The required local ignored `cufft64_12.dll` came from the retained candidate workspace and has SHA-256:

```text
2480D8AB849D7E9A375275F6C0278B8764C14AC0C1A3BDACAF256AE4A93C5590
```

## Not implemented

- UI checkbox/input for startup filtering or disabled CountBoundary.
- QSettings and “set as default”.
- `missingTriggerCount` / gap-count semantics and label/tooltip changes.
- Imaging cap or stop-after-configured-count behavior.
- Timeout screenshot/save-folder production integration.
- Ring/CUDA timeout strategy, partial Ring blocks, CUDA algorithm, or angle-modulo changes.
- Completion-protocol or final-PNG changes.

## Hardware/system validation boundary

```text
SESSION_A_SOFTWARE_IMPLEMENTATION = PASS
SESSION_A_AUTOMATED_TESTS = PASS
SESSION_A_WINDOWS_BUILD = PASS
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS = NOT_PROVEN_BY_SESSION_A
```

## Instructions for Session B

- Start from the final remote HEAD of `codex/session-a-round-policy-core-20260917-114452`, not from `main` or the retained integrated candidate.
- Consume the HostOutput/NetworkController setters and Snapshot fields listed above; do not access Normalizer private state.
- Keep startup filtering measured in new distinct physical trigger identities, not packets/cards/sync frames.
- Preserve cache-hit, late-card, CountBoundary, TimeoutBoundary, and RoundIdentity semantics described above.
- Keep UI/QSettings work separate from Session A core behavior.
