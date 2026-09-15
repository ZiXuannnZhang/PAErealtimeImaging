# 物理轮次并发边界收口 — 执行报告

## 结论

```text
CODE_IMPLEMENTED
AUTOMATED_TESTS_PASS
WINDOWS_BUILD_PASS
HARDWARE_VALIDATION_PENDING
```

已完成任务 `TASKS/物理轮次并发边界收口_20260915.md`。本次收口覆盖两条并发边界：

1. 自动保存目录与 `sessionGen` 的发布顺序不再由 UI 定时器或 UI 回调决定；物理边界源在线程内完成目录准备、注册和 generation 发布，之后才允许下一条 `LogicalScan` 读取新代号。
2. Ring stale 快照准入改用 producer `submit_index` 身份域；不再把 attempt count、`svc frame_seq` 或 UI 调度顺序当作 stale cutoff。`ring_reset` 与 `ring_block_ready` 发送共享同一 ZMQ mutex，发送失败时 fail-closed。

未修改 START fence、PREPARING/ARMED/RUNNING 语义、UDP/CUDA 数值重建或二进制文件格式。真实采集卡、FPGA、现场 NIC/GPU 行为仍需硬件复核。

## 任务身份与回执

```text
TASK_DOC              TASKS/物理轮次并发边界收口_20260915.md
IMPLEMENTATION_BRANCH codex/physical-round-normalizer-20260914-043019
CONTINUE_FROM_SHA     dfdbecde74ad86a2b64e70ac9bc95a6a2a3eb264
PREVIOUS_SOURCE_FIX   ec08d729f792811763671d6cee6a60263cd5f9a7
SOURCE_FIX_SHA        a8fe13b1dba06e2556c55bdf236ff6eda96e40a8
RECEIPT_SHA           0b69ab1e85a5f4a5a1a98ca353bd766cf10ce3a1
```

源码从任务书指定的 continuation SHA 继续；没有 reset、rebase 或 force push。`main` 工作区已有的用户改动未触碰。

## A. 自动保存 generation 发布顺序

新增 `paimage::AutoSaveRoundCoordinator`，把低频目录生命周期和热路径读取分开：

```text
reserve candidate
  → prepare directory
  → register generation → boundary binding
  → release-store current generation
  → publish committed event
  → request saver close/flush
```

`DataProcessor` 在入队前通过 `NetworkController::autoSessionGen()` 做 acquire read，`FileSaver` 用同一个 generation 查询已注册目录。目录映射在 release-store 前已写入并受 mutex 保护，因此新代号不会先于目录注册被生产者读取；旧组携带的旧代号继续解析到旧目录。

边界身份为 `(measurementSession, roundGeneration, boundaryKind)`。精确重复以及更旧边界只返回 `alreadyApplied`，不增加目录编号、不重复发布 generation；物理边界 gate 也在 UI/Ring reset 前抢占同一 session/generation，避免重复清零。

生产路径的职责如下：

- `TimeoutBoundary` 在 `PhysicalRoundNormalizer` observer 所在线程提交 coordinator；Qt UI 只接收已提交结果并异步更新 PNG 目录、标签和诊断。
- `CountBoundary` 只登记 pending 边界，等最后一个旧轮快照完成 `blocksPerFrame` 判定后提交，保留 CountBoundary 的最终帧语义。旧 PNG 目录在 commit 前捕获，新代号不会改写最后一帧的保存目标。
- 初始会话和监听重启通过同一 coordinator 先配置基线目录、再 `beginSession`，不再由 `m_autoSaveNext` 或 `advanceAutoSession` 负责编号。
- `requestCloseSavers()` 在 PAimage HostOutput 路径也会请求各 `FileSaver` 排空并落盘，非 PAimage 路径保持原保存器请求。

目录创建/注册失败时不插入 mapping、不发布候选 generation，而是发布 `kFailedGeneration` 哨兵并关闭 coordinator。UI 失败处理先停止 producer/consumer 写入，再停用自动保存；停用后仍保留失败哨兵直到下一次 `configure()`，避免异步停写窗口落回上一目录。缺失/空基线目录也 fail-closed。

## B. Ring stale cutoff 身份域

producer 每次真正准备 `ring_block_ready` 时由 `RingShmObservability::Tracker` 保留单调 `submit_index`。该 index 通过 ZMQ JSON 传入 ImagingSvc，再原样放入 `ring_snapshot_ready`，由 `ImagingController` 的常驻 worker 和 `ringSnapshotReady` signal 继续传到 UI。

```text
producer submit_index
  → ring_block_ready
  → ImagingSvc processRingPulse
  → ring_snapshot_ready.submit_index
  → UI admitSnapshot(submit_index)
```

`svc frame_seq` 仍仅用于显示、SHM 连续性和诊断；它不参与 stale cutoff。`RingRoundUiState` 的 cutoff 是显式 producer index：

- `recordSubmitIndex()` 只记录真实 reserved identity；非阻塞 send 失败形成 gap，不能把 attempt count 当成有效 snapshot 数。
- `ring_reset` 和 `ring_block_ready` 都通过 `ImagingController::m_zmqMutex` 串行发送。reset send 成功后读取 producer high-water mark 并 arm cutoff；send 失败则阻止后续 snapshot admission，等待 service lifecycle 重新建立。
- snapshot 缺失 `submit_index` 时直接释放 buffer、记 `ring_snapshot_identity_missing`，不计数、不重绘；`submit_index <= cutoff` 的迟到旧快照同样释放并记 `physical_round_stale_frame_dropped`。
- ImagingSvc 停止、重启和 Ring assembler reconfigure 都清理 admission block/cutoff/state；生命周期 reset 不继承旧身份域。

这使 delayed OLD snapshot 不会改变新轮 `frameCount`、`blockCount` 或图像；新轮立即到达的 identity 不会因历史 attempt count 或 SHM `frame_seq` 差异被误丢。CountBoundary 仍由最后一帧准入后的本轮 snapshot 数完成 frame-end/reset。

## 验收覆盖 A1–A6 / B1–B7

新增/更新的 contract tests：

`auto_save_round_coordinator_test` 覆盖：

- A1：TimeoutBoundary commit 在 UI 回调前完成；下一条立即创建的 LogicalScan 读取新 generation。
- A2：旧 backlog 仍写旧目录，新 backlog 写新目录，文件内容和触发数不交叉。
- A3：observer/timer/UI 对同一 boundary 重复调用只分配一次目录，事件顺序为 reserve/commit/已应用。
- A4：generation 读取与目录 resolver 在并发边界中使用同一身份，UI 不构成 authority。
- A5：目录 preparer 失败进入哨兵；新组被丢弃，不回退到旧目录；停用后哨兵仍保留。
- A6：disable/reconfigure 清理旧 binding，空基线目录不启用，新的 lifecycle 使用新的磁盘编号范围。

`ring_round_ui_state_test` 覆盖：

- B1/B2：producer index cutoff 与 send gap；合法的新 identity 保持可准入。
- B3：迟到旧 snapshot 不推进任何新轮计数。
- B4/B5：新旧 identity 交错时只按 producer identity 准入。
- B6：service/session reset 清理旧 cutoff。
- B7：CountBoundary 不提前清零，仍由最终 frame-end 处理。

既有 `filesaver_round_boundary_test` 的历史注释同步改为 coordinator 语义；原有 FileSaver/HostOutput/normalizer 行为断言保持并重跑。

## 验证命令与结果

工具链：Qt 6.8.0 `mingw_64`、MinGW 13.1、Ninja、CMake。

测试配置和编译：

```powershell
cmake -S MC_410T_MultiCard/delivery/tests `
  -B MC_410T_MultiCard/delivery/build/paimage_tests8 `
  -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_CXX_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe `
  -DQt6_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6
ninja -C MC_410T_MultiCard/delivery/build/paimage_tests8
```

结果：configure 通过；测试工程 `[235/235]` 通过，包含 `network_diagnostics_test` 对新 coordinator/NetworkController 链路的编译链接。

最终工作树按单测试 CTest invocation 串行执行 37 个注册目标，使用 Qt/MinGW runtime PATH；每个 invocation 均退出 0：

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
filesaver_round_boundary_test
ring_round_ui_state_test
auto_save_round_coordinator_test
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

汇总：

```text
TOTAL=37 FAILED=0
```

对应执行形式为：

```powershell
cmake -E env `
  "PATH=D:/Qt/Qt6.8.0/6.8.0/mingw_64/bin;D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin;C:/Windows/System32;C:/Windows" `
  ctest --test-dir MC_410T_MultiCard/delivery/build/paimage_tests8 `
  -R '^<test-name>$' --output-on-failure -j1
```

一次性运行全部 CTest 进程时，Windows loader 曾报告部分 Qt targets `0xc0000135`；同一目标在上述隔离 invocation 下全部通过，未观察到测试断言失败。该环境性 launcher 现象不改变 37/37 的隔离回归结果。

Windows MinGW Debug 主工程：

```powershell
cmake --preset mingw-debug
ninja -C MC_410T_MultiCard/delivery/build/mingw_debug PAimageReceiverDiagnostics.exe ImagingSvc
```

结果：`PAimageReceiverDiagnostics.exe` 链接通过，`ImagingSvc` 目标无待构建项；退出码 0。Qt deployment 输出的既有 `catalogs.json`、`dxcompiler.dll/dxil.dll` 和 runtime path 警告不构成编译失败。

## 改动文件

```text
MC_410T_MultiCard/delivery/CMakeLists.txt
MC_410T_MultiCard/delivery/include/ImagingController.h
MC_410T_MultiCard/delivery/include/MainWindow.h
MC_410T_MultiCard/delivery/include/NetworkController.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/AutoSaveRoundCoordinator.h
MC_410T_MultiCard/delivery/include/RingRoundUiState.h
MC_410T_MultiCard/delivery/src/ImagingController.cpp
MC_410T_MultiCard/delivery/src/ImagingSvc/ImagingSvc.cpp
MC_410T_MultiCard/delivery/src/ImagingSvc/ImagingSvc.h
MC_410T_MultiCard/delivery/src/MainWindow.cpp
MC_410T_MultiCard/delivery/src/NetworkController.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/AutoSaveRoundCoordinator.cpp
MC_410T_MultiCard/delivery/src/RingRoundUiState.cpp
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tests/auto_save_round_coordinator_test.cpp
MC_410T_MultiCard/delivery/tests/filesaver_round_boundary_test.cpp
MC_410T_MultiCard/delivery/tests/ring_round_ui_state_test.cpp
```

## 硬件与现场边界

本次自动化验证包含真实 Windows 本机 WinSock loopback 相关测试、软件 HostOutput/FileSaver 路径和 ImagingSvc 编译；没有替代实际采集链路。以下项目仍为 `HARDWARE_VALIDATION_PENDING`：

- 真实 FPGA/NIC 连续 `ring_block_ready`、非阻塞 ZMQ 背压、丢通知和 reset FIFO 时序；
- 真实 CUDA Ring snapshot 在 reset 前后迟到时的 SHM buffer 释放与显示行为；
- 现场权限/防火墙、多卡负载、长时间磁盘 I/O 和真实目录创建失败注入；
- 物理采集卡/GPU 上的最终帧与 partial round 现场回归。

## 提交关系

```text
SOURCE_FIX_SHA a8fe13b1dba06e2556c55bdf236ff6eda96e40a8
RECEIPT_SHA    0b69ab1e85a5f4a5a1a98ca353bd766cf10ce3a1
```

源代码提交与本报告提交分离；回填 SHA 的后续提交也只修改本报告字段。
