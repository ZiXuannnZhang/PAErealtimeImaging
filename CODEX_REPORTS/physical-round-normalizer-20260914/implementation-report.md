# 物理轮次归一化与首控制触发安全过滤 — 执行报告

## 结论

```text
CODE_IMPLEMENTED
AUTOMATED_TESTS_PASS
WINDOWS_BUILD_PASS
HARDWARE_VALIDATION_PENDING
```

已按实施授权追加采用 `first-visible-operational` heuristic：每个 measurement session 和后续 physical round 的第一个新的可见 `triggerSeq` 被视为 `OperationalStartupControl`，从生产 save/Ring logical path 过滤；随后按配置接收 logical scan。此实现不宣称 wire-level control identity，也没有修改 UDP admission 或 START fence。

## 任务身份与回执

```text
ADDENDUM_TASK       TASKS/物理轮次归一化实施授权追加_20260914-110200.md
IMPLEMENTATION_BRANCH codex/physical-round-normalizer-20260914-043019
CONTINUE_FROM_SHA   0752819ba1e63e0127e9d6cbcfe9a305e22bff31
FINAL_SOURCE_SHA    b5260c8eed92178b61bd15834cf45de5f675c16f
FINAL_RECEIPT_SHA   pending-report-commit
LOCAL_HEAD_EQUALS_REMOTE_HEAD  verified after final push
```

源代码提交从任务书指定的 `CONTINUE_FROM_SHA` 继续，没有 reset、rebase 或 force push。原来的 `PROTOCOL_BLOCKED` 报告已由本实施报告替换。

## 实现要点

### Operational policy 与共享分类

- 新增 `paimage::PhysicalRoundNormalizer`，状态为 `AwaitingControl` / `CollectingScan`。
- 新 measurement session 清空 logical count 和 recent decision cache；首个新的 identity 过滤为 `OperationalStartupControl`。
- 一个 identity 的 key 是 `measurementSession + uint16 triggerSeq`，并使用容量为 8192 的 bounded recent-decision cache。跨卡乱序、迟到卡和 `uint16` wrap 复用同一个 decision；同一 identity 只推进一次 logical count。
- count boundary 在第 N 个 logical identity 上产生一次 `roundComplete`/`CountBoundary`；同一 identity 的迟到卡复用上一轮 logical index，不重复 reset，也不变成下一轮 control。
- classification 与 `CardFrame::complete` 无关，因此 partial 的首 control 也会被过滤；其底层 SourceCore physical partial/loss 统计继续保留。

### 配置来源

- 线性/默认路径使用持久化 `AcquisitionParams/LogicalTriggersPerRound`，默认值由 `AcqConfig::kDefaultLogicalTriggersPerRound` 提供（4000）。normalizer 本身不内置 4000。
- Ring 路径使用 canonical source `RingReconCudaConfig.alinesPerFrame / enabledChannelCount`，启动时校验它等于 `2 * alinesPerChannelPerFrame`，并校验 logical round 可整除 `enabledChannelCount * alinesPerChannelPerBlock`；不一致时记录 `imaging_assembler_invalid_config` 并拒绝配置组包器。
- Ring 配置通过 `NetworkController::setLogicalTriggersPerRound` 更新同一个 production normalizer；没有新增互不关联的 UI counter。

### Save、Ring 与边界行为

- 分类位于 `HostOutput` 的 production output boundary，先于 FileSaver 和 Ring；control 不进入正常 FileSaver 数据流、不进入 Ring、不推进 wavelength parity、angle/index、block 或 logical count。
- 同一 `FrameConverter` entry 将 classification metadata 复制到 `TriggerGroup`，save 与 sync/Ring 共用同一 decision 和 logical index。
- Ring consumer 在最终 logical identity 到达后，按 enabled-card mask 等待全部启用卡的同一 identity 消费完成，再调用 `RingBlockAssembler::completeLogicalRound()` 一次；这样多卡首卡先到不会提前重置，迟到卡仍可进入上一 identity。
- count boundary 重置 Ring host 侧 wavelength/angle phase；`blockSeq` 保持单调。由于 N 与 block 配置已校验，ImagingSvc 的现有 blocks-per-frame/reconstruction reset 与 logical round 对齐，auto-save 的圈末条件也继续按 logical block/frame 边界工作。
- timeout boundary 清空 partial normalizer state、`ImagingBypass` 队列和 Ring partial block，并通过既有 callback 发出 reconstruction `ring_reset`；normalizer 在已处于 `AwaitingControl` 时不重复推进 generation 或产生空 round。

## 诊断与已接受风险

每次 normalizer boundary snapshot/事件包含：

```text
roundGeneration
configuredLogicalTriggersPerRound
physicalDistinctObserved
operationalControlFiltered
logicalDistinctAccepted
countBoundaryResets
timeoutBoundaryResets
firstVisibleFilterMode=true
```

每次控制过滤记录 `paimage.round / round_control_filtered`，包含 `measurementSession`、`roundGeneration`、`physicalTriggerSeq`、`basis=first-visible-operational`，以及 `filterClassification=software-operational`、`filterIsNotNetworkLoss=true`、`packetLossAccounting=unchanged`。SourceCore 的 raw trace 仍记录真实 trigger、packet、卡号、complete/partial decision 和原始原因；control filter 不计入 `packetsDropped`、`stale` 或 START fence discard。

若真实 control 在 ingress 前完全丢失，按授权契约软件仍会过滤第一条可见 scan；这是明确记录的 `acceptedControlInvisibleRisk`，不是网络丢包结论，也不是声称 physical trigger 不存在。笔记本 startup ingress loss 未在本分支解决；其独立诊断基线未修改。

## 证据与测试

### Normalizer / 集成证据

`physical_round_normalizer_test` 覆盖 T1–T12，包括小 N、生产 N=4000、连续三轮、跨卡共享 identity、迟到卡、partial control、部分/全部 control 不可见的预期行为、timeout、count+timeout 幂等和 `65534/65535/0/1` wrap。N=4000 断言：

```text
physicalDistinctObserved = 4001
operationalControlFiltered = 1
logicalDistinctAccepted = 4000
countBoundaryResets = 1
```

`paimage_host_output_test` 断言 control 不进入 save/Ring，N=3 的 save 与 Ring identity 均为 101/102/103，logical index 均为 0/1/2，边界标记只在 final logical identity 上出现一次，并校验 float16 文件不含 control 数据。

`paimage_network_test` 运行四卡、三轮 source-backed UDP 回放；非 missing 场景每张卡的 Ring identity 都是 trigger 1..21、logical index 0..20，累计 `hostRingReturns=252`，保存文件大小按 21 个 logical trigger 计算。现有 START/session、batch、Ring、socket、trace、worker、protocol、conversion 和 looplog 回归均通过。

### Exact commands

源码最终 SHA：`b5260c8eed92178b61bd15834cf45de5f675c16f`。

```powershell
Set-Location D:\ChatGPT\PAERealtimeImaging\_worktrees\physical-round-normalizer-20260914-043019\MC_410T_MultiCard\delivery
cmd /c build_mingw_debug.cmd configure
cmd /c build_mingw_debug.cmd build
```

结果：CMake Debug configure 通过；Windows MinGW Debug 主程序链接通过，产物为 `build/mingw_debug/bin/PAimageReceiverDiagnostics.exe`。configure/post-build 仅报告环境中缺少 Vulkan headers、Qt translations catalog、DX compiler 的 warning，没有构建失败。

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
$env:Path = 'D:\Qt\Qt6.8.0\6.8.0\mingw_64\bin;D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin;D:\Qt\Qt6.8.0\Tools\Ninja;' + $env:Path
& 'D:\Qt\Qt6.8.0\Tools\Ninja\ninja.exe' -C D:\ChatGPT\PAERealtimeImaging\_worktrees\physical-round-normalizer-20260914-043019\MC_410T_MultiCard\delivery\build\paimage_tests8
```

结果：测试目标增量重建通过。

测试在上述 Qt/offscreen 与 MinGW PATH 下逐项隔离执行以下命令形式：

```powershell
& 'D:\Qt\Qt6.8.0\Tools\CMake_64\bin\ctest.exe' -R '^<test-name>$' --output-on-failure
```

34 项全部通过：

```text
ring_block_assembler_test
imaging_bypass_test
data_processor_imaging_isolation_test
diagnostic_recorder_test
ring_shm_observability_test
diagnostic_dialog_test
diagnostic_time_window_test
process_scheduling_test
network_ingress_observability_test
network_snapshot_test
network_diagnostics_test
measurement_session_transaction_test
session_boundary_test
session_boundary_receiver_test
data_processor_batch_test
card_status_formatting_test
paimage_network_test
paimage_start_race
paimage_start_fence_regression
paimage_start_overflow
paimage_host_output_test
paimage_trace_bundle_test
paimage_discovery_checks
paimage_discovery_socket_checks
paimage_core_checks
paimage_output_checks
physical_round_normalizer_test
paimage_control_checks
paimage_socket_short
paimage_trace_checks
paimage_worker_checks
paimage_protocol_checks
paimage_conversion_checks
paimage_looplog_checks
```

结果：`PASS isolated CTest 34 tests at final source SHA`。

## 修改文件

生产实现：

```text
MC_410T_MultiCard/delivery/CMakeLists.txt
MC_410T_MultiCard/delivery/include/AcqConfig.h
MC_410T_MultiCard/delivery/include/DataTypes.h
MC_410T_MultiCard/delivery/include/MainWindow.h
MC_410T_MultiCard/delivery/include/NetworkController.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/Backend.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/FrameConverter.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/HostOutput.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/PhysicalRoundNormalizer.h
MC_410T_MultiCard/delivery/include/RingBlockAssembler.h
MC_410T_MultiCard/delivery/src/MainWindow.cpp
MC_410T_MultiCard/delivery/src/NetworkController.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/Backend.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/FrameConverter.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/HostOutput.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/PhysicalRoundNormalizer.cpp
MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp
```

测试与构建：

```text
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tests/paimage_core/CMakeLists.txt
MC_410T_MultiCard/delivery/tests/paimage_core/physical_round_normalizer_test.cpp
MC_410T_MultiCard/delivery/tests/paimage_host_output_test.cpp
MC_410T_MultiCard/delivery/tests/paimage_network_test.cpp
MC_410T_MultiCard/delivery/tests/paimage_trace_bundle_test.cpp
MC_410T_MultiCard/delivery/tests/ring_block_assembler_test.cpp
```

`paimage_trace_bundle_test.cpp` 的变更只同步其已有 LoopLog/启动分析器/抓取 launcher fixture 与当前仓库契约，未改变生产 trace 语义。

## 限制与硬件状态

- 当前没有本次任务的台式机实机采集时段；`physical 4001 -> logical/save/Ring 4000` 已由配置驱动的 N=4000 contract test 和 source-backed 多卡集成测试覆盖，但仍需台式机硬件验证连续多轮 frame/save/reconstruction boundary。
- 主程序 configure/build 通过；Vulkan headers、Qt translations catalog 和 DX compiler 仍是当前工作站部署环境的非致命缺项。
- 构建期间临时补入的 ignored `libs/imaging/cufft64_12.dll` 已在验证后删除；没有留下新的追踪临时文件。
- 不得将本报告解释为已修复笔记本 startup ingress 丢包；本分支只落实授权的 operational first-visible filter。

最终推送后应核对并保持：

```text
git rev-parse HEAD == git rev-parse origin/codex/physical-round-normalizer-20260914-043019
```
