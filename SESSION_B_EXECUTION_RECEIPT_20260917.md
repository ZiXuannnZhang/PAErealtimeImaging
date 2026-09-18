# Session B Execution Receipt — 2026-09-17

## Scope

This receipt covers `TASKS/SessionB_配置持久化与状态诊断UI_20260917.md`
(Session B of the serial round-policy rework):

1. `startupFilterTriggerCount` ring-scan UI + current runtime config +
   "设为默认/恢复默认" persistence;
2. independent `disableCountBoundary` checkbox + the same persistence chain;
3. `missingTriggerCount`（跳号数）and the frozen
   `缺失 / 跳号数 / 已采集 / 已过滤 / 丢包` status semantics.

Session C variable-length round production closing behavior is explicitly not
implemented here.

## Git identity

```text
repository = https://github.com/ZiXuannnZhang/PAErealtimeImaging.git
task document branch = codex/task-docs
implementation branch = codex/session-b-ui-observability-20260917
baseline branch = codex/session-a-round-policy-core-20260917-114452
starting SHA = eb283f64574d9043f4d4823338c31767c3b811fe
  (verified: git ls-remote origin refs/heads/codex/session-a-round-policy-core-20260917-114452
   returned exactly this SHA before branching)
implementation commit = 917a71986d01316a8dcb6a2707739b2a7be29d27
receipt commit = reported in the final execution report to avoid self-reference
remote branch HEAD after final push = reported out-of-band in the final execution report
```

Tracked working tree at the code-validation point (after the implementation
commit, before builds):

```text
git status --porcelain --untracked-files=no
(empty)
```

Untracked local-only inputs, not staged: the ignored build trees
(`MC_410T_MultiCard/delivery/build/`) and the ignored imaging runtime
`MC_410T_MultiCard/delivery/libs/imaging/cufft64_12.dll` (source/hash below).

## Implementation summary

- `include/RoundPolicySettings.h` (new): single-persistence-source helper for
  the round startup policy. `load(QSettings&)` / `save(QSettings&, Policy)` /
  `loadDefault()`; group `RingConfigDialog/Defaults`, keys
  `startupFilterTriggerCount` / `disableCountBoundary`; factory/no-history
  defaults 1 / false; the two keys are independent.
- `RingConfigDialog.{h,cpp}`: `启动过滤触发数` spinbox (0..1000000, factory 1)
  and `禁用计数重置` checkbox (factory false) in the 扫描参数 tab; both wired
  into the existing `RingConfigDialog/Defaults` restore/save chain via the
  helper; `applyConfig()` emits `roundPolicyChanged(quint64, bool)` on success.
- `MainWindow.{h,cpp}`: `startListeningWithIPs()` applies
  `RoundPolicySettings::loadDefault()` to `NetworkController` right after
  construction (backend not yet created; values reach `Backend::Settings` via
  `createPaimageBackend()`), so a saved default survives program start without
  opening the dialog. `ensureRingConfigDialog()` is the single lazy-create
  entry that forwards `roundPolicyChanged` to
  `NetworkController::setStartupFilterTriggerCount / setDisableCountBoundary`.
- `DataTypes.h`: `CardStats::missingTriggerCount` atomic hot field +
  `Snapshot` field + `snapshot()`.
- `DataProcessor.cpp`: `missingTriggerCount += skipGap` in the trigger-switch
  path and `+= gap` in the empty-buffer/last-flushed anchor path (the two
  existing mutually exclusive full-trigger gap entries; `didSwitch` prevents
  double counting). Packet-loss decisions unchanged.
- `NetworkController.cpp`: `runtimeStatsFields()` gains `missingTriggerCount`.
- `CardStatusFormatting.h`: `RoundDisplay{collected, filtered}`,
  `roundDisplayForUi(Snapshot)` (current>0 else lastCompleted fallback),
  frozen text `卡%1 | 缺失: %2 | 跳号数: %3 | 已采集: %4`, tooltip gains
  `已过滤` and renames the misleading `不完整触发缺包数` line to `丢包`
  (data unchanged).
- `MainWindow::onUpdateStatistics()` reads
  `m_netController->physicalRoundSnapshot()` at most once per refresh tick and
  passes the resulting `RoundDisplay` to per-card formatting.

## Exact test commands and results

### Test-tree configure (fresh build dir in this worktree)

```powershell
& 'D:\Qt\Qt6.8.0\Tools\CMake_64\bin\cmake.exe' -S tests -B build/tests_all_mingw_debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe `
  -DQt6_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6 `
  -DQt6CoreTools_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6CoreTools `
  -DQt6GuiTools_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6GuiTools `
  -DQt6WidgetsTools_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6WidgetsTools
```

Result: exit 0 (the explicit `Qt6*Tools_DIR` variables mirror the Session A
cache; without them Qt6Widgets' nested tool dependency lookup fails).

### Test build

```powershell
& 'D:\Qt\Qt6.8.0\Tools\CMake_64\bin\cmake.exe' --build build/tests_all_mingw_debug -j 8
```

Result: exit 0, 267/267 targets built.

### Full ctest suite (includes all Session A regressions)

Run from `build/tests_all_mingw_debug` with Qt 6.8.0 mingw_64 and MinGW
13.1.0 prepended to `PATH`:

```powershell
& 'D:\Qt\Qt6.8.0\Tools\CMake_64\bin\ctest.exe' --output-on-failure -j 4
```

Result: `100% tests passed, 0 tests failed out of 41` (34.07 s). The suite
includes `physical_round_normalizer_test`, `paimage_host_output_test`,
`ring_production_blockers_test`, `ring_block_assembler_test`,
`ring_round_identity_test`, `count_boundary_save_binding_test`,
`filesaver_round_boundary_test`, `auto_save_round_coordinator_test`,
`network_diagnostics_test`, `data_processor_batch_test`,
`card_status_formatting_test`, `round_policy_settings_test` and the remaining
paimage/session/ring tests.

### Direct execution of the Session B focal tests

```powershell
.\data_processor_batch_test.exe      # exit 0, "PASS DataProcessor batch/counter tests"
.\card_status_formatting_test.exe    # exit 0, "PASS card status formatting"
.\round_policy_settings_test.exe     # exit 0, "PASS round policy settings"
.\network_diagnostics_test.exe --legacy-control-only   # exit 0
```

Coverage mapping:

- B1: `testMissingTriggerSwitchPath` — T100(partial 3/6) -> T104:
  `missingTriggerCount += 3` exactly once, `triggersPartial == 1`,
  `packetsDropped == 3 + 3*6` (partial missing packets + full-gap packet
  equivalents).
- B2: `testMissingTriggerPartialOnly` — partial-only switch:
  `missingTriggerCount == 0`, `packetsDropped == 2`, `triggersPartial == 1`.
- B3: `testMissingTriggerEmptyBufferGapPath` — T100 -> T104 -> T108 with
  empty buffer between completions: `missingTriggerCount == 6` (3+3),
  `packetsDropped == 6`, no double counting.
- B4: `testMissingTriggerWrapForward` / `testMissingTriggerResetRecovery` —
  uint16 wrap-forward gap counts exactly 2 (T65535/T0), small backstep and
  >=256 back-jump reset recovery add 0.
- Formatting: frozen line `卡1 | 缺失: 3 | 跳号数: 4 | 已采集: 4007`;
  tooltip contains `已过滤: 7` and `丢包: 7`, no `不完整触发缺包数`;
  `roundDisplayForUi` fallback (current==0 -> lastCompleted 4007/7,
  current>0 -> current 12/3).
- Settings: no history -> 1/false; save 7/true -> reload 7/true; restore
  reads the saved values; X=0 + disable=false and X=7 + disable=true stay
  independent; keys verified inside `RingConfigDialog/Defaults`; tests use a
  temporary INI only.
- Propagation: `network_diagnostics_test::roundPolicyPropagation()` —
  controller defaults 1/false; `setStartupFilterTriggerCount(7)` +
  `setDisableCountBoundary(true)` reflected in `physicalRoundSnapshot()`;
  X=0 does not alter disable and vice versa. Combined with the settings
  helper test, this covers (1) saved default readable before dialog
  lazy-create and applied at controller/backend creation, and (2) Apply/OK
  forwarding through the same setters, without a large UI automation frame.

## Windows build

Command executed from `MC_410T_MultiCard/delivery` at the implementation
commit `917a71986d01316a8dcb6a2707739b2a7be29d27` (full configure + build,
per BUILD_STANDARD §2.3):

```powershell
cmd /c build_mingw_debug.cmd
```

Result: exit 0. The script ran `mingw-debug` configure + `mingw-debug-build`
build and `windeployqt` deployment using:

```text
Qt = D:\Qt\Qt6.8.0\6.8.0\mingw_64
MinGW = D:\Qt\Qt6.8.0\Tools\mingw1310_64 (13.1.0)
CMake = 3.30.5
Ninja = 1.12.1
code SHA = 917a71986d01316a8dcb6a2707739b2a7be29d27
```

Required output check (script-internal, exit 3 on failure — passed):

```text
build/mingw_debug/bin/PAimageReceiverDiagnostics.exe  present, 65277163 bytes
build/mingw_debug/bin/ImagingSvc.exe                  present, 3338451 bytes
build/mingw_debug/bin/ring_svc_selftest.exe           present, 1941820 bytes
build/mingw_debug/bin/ring_udp_replay.exe             present, 333547 bytes
```

`build/mingw_debug/bin/diagnostic-tools/` is present, and the runtime set
(`libzmq-v141-mt-4_3_5.dll`, `cudart64_12.dll`, `cufft64_12.dll`,
`pa_recon_core.dll`, `ring_recon_cuda.dll`) is deployed in `bin/`.

## Dependency receipt

Session B changed no CUDA/Ring code; the build reused the tracked prebuilt
Ring CUDA runtime and the existing imaging runtime:

```text
Ring CUDA source = _migration_pack/prebuilt_cuda/bin (tracked, fallback path)
ring_recon_cuda.dll       SHA256 = BF40472D5A46363A35DD1084A01A8C15F13EF203F3ED5B4BB5EDF767EEEC14B5
libring_recon_cuda.dll.a  SHA256 = 94ABAB973582D06DEBF3831366BAEC00458EA3277F5F3B852CB1FA24A3C3F5
cudart64_12.dll           SHA256 = C2C9A9C22A9BCBA90E261825968836787B331038047A26770CFFB7A583C28344
pa_recon_core.dll         SHA256 = C1A37E73147C1B8250F96CCDC1FCFC08CA28CECBF1CB3C046D4AB426F2EA0893
libzmq-v141-mt-4_3_5.dll  SHA256 = 37610023D91951BC4177DB1F96B54B28911976AC70B221A852887709A02A9AD3
```

The ignored imaging runtime `cufft64_12.dll` (required by
`pa_recon_core.dll`, not tracked by Git) was copied into this worktree from
the retained Session A candidate worktree:

```text
source = _worktrees/session-a-round-policy-core-20260917-114452/MC_410T_MultiCard/delivery/libs/imaging/cufft64_12.dll
SHA256 = 2480D8AB849D7E9A375275F6C0278B8764C14AC0C1A3BDACAF256AE4A93C5590
(matches the value recorded in the Session A receipt)
```

## Changed files

```text
MC_410T_MultiCard/delivery/include/RoundPolicySettings.h        (new)
MC_410T_MultiCard/delivery/include/CardStatusFormatting.h
MC_410T_MultiCard/delivery/include/DataTypes.h
MC_410T_MultiCard/delivery/include/MainWindow.h
MC_410T_MultiCard/delivery/include/RingConfigDialog.h
MC_410T_MultiCard/delivery/src/DataProcessor.cpp
MC_410T_MultiCard/delivery/src/MainWindow.cpp
MC_410T_MultiCard/delivery/src/NetworkController.cpp
MC_410T_MultiCard/delivery/src/RingConfigDialog.cpp
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tests/card_status_formatting_test.cpp
MC_410T_MultiCard/delivery/tests/data_processor_batch_test.cpp
MC_410T_MultiCard/delivery/tests/network_diagnostics_test.cpp
MC_410T_MultiCard/delivery/tests/round_policy_settings_test.cpp (new)
SESSION_B_EXECUTION_RECEIPT_20260917.md                         (this file)
HANDOFF_SESSION_B_20260917.md
```

## Scope and validation boundary

Not done (explicitly out of scope): Session C variable-length production
closing (imaging cap after configured count, timeout screenshot, FileSaver
timeout rollover, new Ring/ImagingSvc timeout reset behavior), any
PhysicalRoundNormalizer state-machine change, any Session A semantics change,
any packet-loss decision change, any hardware validation.

```text
SESSION_B_SOFTWARE_IMPLEMENTATION = PASS
SESSION_B_AUTOMATED_TESTS = PASS (ctest 41/41)
SESSION_B_WINDOWS_BUILD = PASS
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS = NOT_PROVEN_BY_SESSION_B
```
