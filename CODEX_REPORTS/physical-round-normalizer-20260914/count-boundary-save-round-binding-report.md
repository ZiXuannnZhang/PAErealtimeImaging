# CountBoundary 自动保存轮次绑定整改追加 — 执行报告

## 结论

```text
CODE_IMPLEMENTED
AUTOMATED_TESTS_PASS
WINDOWS_BUILD_PASS
HARDWARE_VALIDATION_PENDING
```

已按任务 `TASKS/CountBoundary自动保存轮次绑定整改追加_20260915-200300.md` 完成源码实现、自动化验证和 Windows 构建。CountBoundary 现在在源线程同步准备并发布下一物理轮次的数据绑定；旧轮最后的 TriggerGroup、迟到多卡和旧 PNG 继续使用旧目录，最终帧回调只应用已准备的 presentation target，不再负责分配 generation。

未修改 `PhysicalRoundNormalizer` 状态机、首控制过滤策略、物理超时算法、`FileSaver` 轮次翻滚主体、Ring stale `submit_index` 设计、ZMQ/CUDA/START 链路及二进制保存格式。真实采集卡、FPGA、现场 NIC/GPU 行为仍需硬件复核。

## 任务身份与提交

```text
TASK_DOC              TASKS/CountBoundary自动保存轮次绑定整改追加_20260915-200300.md
IMPLEMENTATION_BRANCH codex/physical-round-normalizer-20260914-043019
CONTINUE_FROM_SHA     a678b9cdc44c48cccbb8bd02d7e5ef1b68275baa
SOURCE_FIX_SHA        32a3071d7be1e4dbe6558832215e0bb559f9f3b3
RECEIPT_SHA           TO_BE_FILLED
```

实现分支 continuation baseline 与任务要求一致；没有 reset、rebase 或 force push。主工作区原有用户改动未触碰。

## Implementation

### Round binding data model

`AutoSaveRoundCoordinator` 新增 `(measurementSession, roundGeneration) → sessionGen → directory` 映射和 `resolveRound()`。映射在目录准备、注册完成后才发布；同一 round identity 始终解析到同一 generation。lookup 缺失、目录缺失或协调器故障均返回 `kFailedGeneration` 哨兵并低频记录 `LookupFailed`，不回退到 current generation、previous directory 或 base directory。

### Initial/lifecycle binding

`HostOutput::beginSession()` 先调用 measurement-session binder，再启动 normalizer 和输出 worker，因此 round 0 在第一条 LogicalScan 的 save stamp 前已经绑定，覆盖 N=1。source restart/resume 重新绑定并清理旧 round table；auto-save disable/reconfigure 清理旧映射。测量进行中从 manual 切到 auto-save 时，绑定当前 normalizer round，而不是错误地只绑定 round 0。

### CountBoundary ordering and save stamp

CountBoundary observer 在返回前执行：

```text
reserve next generation
→ prepare/create directory
→ register generation and round binding
→ publish binding
```

`HostOutput` 对 normalized frame 按自身的 `(measurementSession, roundGeneration)` 调用 resolver 后再打 `sessionGen`；生产 PAimage 路径不以全局 current generation 决定 round 路由。旧轮最后 identity 的迟到卡仍命中旧映射，下一轮 LogicalScan 可以在旧 final snapshot 尚未返回时直接写新目录。

### Final frame/presentation and timeout

CountBoundary commit 结果携带 old/new directory，MainWindow 将结果放入 pending presentation queue。旧 final frame 的 PNG 使用 `oldDirectory/recon_png`，其后只调用 presentation apply；`AutoSavePresentationState` 按 session/round/generation 做单调性检查，迟到旧 callback 记录 `auto_save_presentation_transition_stale_ignored` 且不倒退 UI 目标、不重复分配目录。TimeoutBoundary 复用同一 round-aware resolver，并保留原有 Ring reset、stale snapshot 和 UI reset 时序。

### Observability

coordinator/NetworkController 保留并补充以下低频状态：`prepared`、`committed`、`lookup_failed`、`count presentation transition pending`、`count presentation transition applied`、`stale presentation transition ignored`，字段包含 `measurementSession`、`roundGeneration`、`oldSessionGen`、`newSessionGen`、`resolvedSessionGen`、`oldDirectory`、`directory`、`boundaryKind` 和 `phase`。没有增加 per-packet 日志。

## Tests

### C1–C10 专项测试

新增自动化目标：`count_boundary_save_binding_test`，执行命令（在 `MC_410T_MultiCard/delivery` 下）：

```powershell
& 'D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe' -C build/paimage_tests8 count_boundary_save_binding_test
$env:Path = 'D:/Qt/Qt6.8.0/6.8.0/mingw_64/bin;D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin;C:/Windows/System32;C:/Windows;' + $env:Path
& '.\build\paimage_tests8\count_boundary_save_binding_test.exe'
```

结果：

```text
count_boundary_save_binding_test: C1-C10 ALL PASS
```

覆盖内容包括：CountBoundary 早于旧 final snapshot、两卡最终 identity 迟到、N=1、三个连续 count-complete rounds、saver backlog 实际 `.dat` 字节与目录、final-frame 不重复分配、stale presentation、TimeoutBoundary、manual generation=0 和 measurement/source/auto-save lifecycle。

`auto_save_round_coordinator_test` 同时新增了故障后的 round lookup fail-closed 断言；结果 PASS。

### 全量注册回归

最终 CMake 注册检查：

```powershell
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/ctest.exe' --test-dir 'MC_410T_MultiCard/delivery/build/paimage_tests8' -N
```

结果：`Total Tests: 38`。相较 continuation baseline 新增 `count_boundary_save_binding_test` 1 项。

最终逐项隔离执行命令模板如下；每个 test name 都独立启动一个 CTest 进程：

```powershell
$ctestPath = 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/ctest.exe'
$env:Path = 'D:/Qt/Qt6.8.0/6.8.0/mingw_64/bin;D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin;C:/Windows/System32;C:/Windows;' + $env:Path
& $ctestPath --test-dir 'build/paimage_tests8' -R '^<test-name>$' --output-on-failure -j1
```

执行的 38 个目标：

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
count_boundary_save_binding_test
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

结果：

```text
PASSED=38 FAILED=0
```

`git diff --check` 通过；Git 仅提示工作树 LF/CRLF 转换警告，没有 whitespace error。

## Build

工具链：Qt 6.8.0 `mingw_64`、MinGW 13.1、CMake、Ninja。

测试工程最终 reconfigure：

```powershell
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe' -S tests -B build/paimage_tests8 -G Ninja
```

结果：PASS。配置输出包含 `Could NOT find WrapVulkanHeaders (missing: Vulkan_INCLUDE_DIR)`，随后 `Configuring done`、`Generating done`，没有配置失败。

测试工程构建：

```powershell
& 'D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe' -C build/paimage_tests8
```

结果：PASS，最终源码下 `ninja: no work to do`；目标已经在前序增量构建中完成编译链接。

Windows Debug 主工程最终 reconfigure：

```powershell
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe' -S . -B build/mingw_debug -G Ninja
```

结果：PASS。相同 Vulkan include 提示不影响生成。

主程序与 ImagingSvc：

```powershell
& 'D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe' -C build/mingw_debug PAimageReceiverDiagnostics.exe ImagingSvc
```

结果：PASS，`PAimageReceiverDiagnostics.exe` 已按最终源码重新链接，`ImagingSvc` 无待构建项（exit code 0）。windeployqt 的 `catalogs.json`、`dxcompiler.dll/dxil.dll` 和 runtime path 提示是本机部署环境警告，不是编译/链接失败。

## Changed files

```text
MC_410T_MultiCard/delivery/include/MainWindow.h
MC_410T_MultiCard/delivery/include/NetworkController.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/AutoSaveRoundCoordinator.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/HostOutput.h
MC_410T_MultiCard/delivery/src/MainWindow.cpp
MC_410T_MultiCard/delivery/src/NetworkController.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/AutoSaveRoundCoordinator.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/HostOutput.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tests/auto_save_round_coordinator_test.cpp
MC_410T_MultiCard/delivery/tests/count_boundary_save_binding_test.cpp
```

## Known limitations / hardware

```text
HARDWARE_VALIDATION_PENDING
```

本次验证覆盖 Windows 本机软件 coordinator/HostOutput/FileSaver 路径、PAimage/Ring/save 回归、START/session 回归和 Windows Debug 构建；未替代以下现场验证：真实 FPGA/NIC 多卡迟到卡时序、非阻塞 ZMQ 背压与丢通知、真实 CUDA snapshot 在 reset 前后迟到时的行为、现场磁盘/权限/防火墙和长时间负载。

## Local / remote HEAD

```text
LOCAL_HEAD_AT_SOURCE_COMMIT 32a3071d7be1e4dbe6558832215e0bb559f9f3b3
REMOTE_HEAD                  TO_BE_VERIFIED_AFTER_PUSH
```

回执提交后将回填 `RECEIPT_SHA`、最终 branch HEAD 和远端 HEAD；源码提交与 report-only 提交保持分离。
