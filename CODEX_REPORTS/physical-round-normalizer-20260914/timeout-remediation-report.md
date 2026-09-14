# 物理轮次超时重置语义整改 — 执行报告

## 结论

```text
CODE_IMPLEMENTED
AUTOMATED_TESTS_PASS
WINDOWS_BUILD_PASS
HARDWARE_VALIDATION_PENDING
```

已完成追加任务 `TASKS/物理轮次超时重置语义整改追加_20260914.md`：物理轮次超时只由 `RingReconCudaConfig.timeoutResetSec` 驱动；`0` 表示禁用。`SourceCore::Decision::Timeout` 仍只表示单卡/单触发组装超时，不再触发物理轮次 boundary。生产路径上的超时会在同一 measurement session 内、下一个新的可见 physical identity 被分类时生效：部分轮次结束，该 identity 被过滤为 `OperationalStartupControl`，下一条 logical identity 从 index 0 开始。

## 任务身份与回执

```text
ADDENDUM_TASK       TASKS/物理轮次超时重置语义整改追加_20260914.md
IMPLEMENTATION_BRANCH codex/physical-round-normalizer-20260914-043019
CONTINUE_FROM_SHA   c28db3d373f378b7ae17e250cd53a2778bc0fe1b
SOURCE_FIX_SHA      0d57baffb59819b13e212aa017c85ca1a408e170
RECEIPT_SHA         pending-report-commit
LOCAL_HEAD_EQUALS_REMOTE_HEAD  待最终推送后核对
```

源码从任务书指定的 continuation SHA 继续，没有 reset、rebase 或 force push。此前已提交的首控制过滤语义保持不变；本追加只修正超时边界的归属和生产触发方式。

## 实现与数据流

- `RingReconCudaConfig.timeoutResetSec` 在 ImagingSvc 未运行时仍从当前 Ring 配置读取，并在创建 PAimage backend 前传播；运行时配置更新也同步传播。
- 数据流为：Ring 配置 → `MainWindow` / `NetworkController::setPhysicalRoundTimeout` → `Backend::Settings` → `HostOutput` → `PhysicalRoundNormalizer`。
- normalizer 使用 `CardFrame::first`，缺失时回退到 `closed`；两者均来自 `SocketReceiver` 的 steady-clock monotonic nanoseconds。只有新的 physical identity 会更新 idle anchor；同一 identity 的缓存命中、迟到卡和乱序卡不会刷新 anchor。
- 分类先检查当前 session 是否处于 `CollectingScan` 以及当前新 identity 的 idle gap，再处理当前 identity。因此超时后的第一个可见 identity 收到一次 `TimeoutBoundary` 和一次 `ControlFiltered`，随后 logical index 从 0 开始。
- 超时诊断包含 `measurementSession`、`roundGeneration`、`idleDurationNs`、`configuredTimeoutNs`、`lastDistinctTriggerSeq`、可用时的 `nextVisibleTriggerSeq`，并固定记录 `basis=physical-idle-timeout`。
- 运行诊断包含：

  ```text
  physicalRoundTimeoutResetSec
  physicalRoundTimeoutEnabled
  physicalRoundTimeoutSource=RingReconCudaConfig.timeoutResetSec
  assemblyTimeoutIsNotRoundBoundary=true
  ```

## 组装超时隔离

`HostOutput` 中删除了 `Decision::Timeout` 到 `PhysicalRoundNormalizer::timeoutBoundary()` 的两处错误调用。SourceCore 仍保留 partial frame、`runtimeIncomplete` 和组装超时观察；原有 packet-loss / incomplete 诊断继续工作，组装超时不会增加 `timeoutBoundaryResets`，不会生成 START 或 packets-dropped 语义。

Ring 生产组装器改为由 normalizer/HostOutput 外部管理超时；独立的 `RingBlockAssembler` 单元测试仍保留内部 timer 的默认行为。Count boundary、physical-idle timeout、save、Ring 和 auto-save 共用 normalizer 的 session/generation 事件及一次性 handoff guard；已在 `AwaitingControl` 时不会二次推进 generation 或重复 reset。

## T1–T7 证据

- T1：`paimage_host_output_test` 通过真实生产样式的 card + sync、save、Ring 路径，配置 `1.0e-6` 秒，时间序列 `1000,1500,1800,20000,...`。检测到 `idleDurationNs=18200`，前一 identity 为 `102`，下一可见 identity 为 `200`；save/Ring 共同收到 `[101,102,201,202,203]`，logical index 为 `[0,1,0,1,2]`，只在最后一个 identity 产生 count boundary。
- T2：normalizer 配置 timeout `0`，长 idle gap 不产生 timeout boundary，下一 identity 不被错误重置。
- T3：`SourceCore` 的约 100ms assembly timeout 仅结束 trigger `12` 的 partial card；证据为 `runtimeIncomplete>=1`、`timeoutBoundaryResets==0`、normalizer logical accepted/current count 为 `3`，Ring 未收到 timed-out partial frame，仅首个可见 operational control 事件被过滤。
- T4：先完成 count boundary，再经过长 idle gap；由于已回到 `AwaitingControl`，不重复生成 timeout boundary 或二次 reset。
- T5：迟到的已缓存 identity 不更新 idle anchor；随后新 identity 按原 anchor 触发 timeout。
- T6：`uint16` wrap（`65535 → 0 → 1`）仍携带正确的上一/下一 trigger identity。
- T7：production HostOutput 的 save、Ring 和 auto-save 共享同一次 timeout/session generation handoff；没有重复 reset 或重复 logical output。

normalizer contract test 最终输出为：

```text
PASS physical round normalizer contract (T1-T17)
```

其中新增超时覆盖对应 normalizer T13–T17；HostOutput 集成测试覆盖生产路径的 T1/T3/T4/T7。

## 验证命令与结果

测试目标构建：

```powershell
ninja -C MC_410T_MultiCard/delivery/build/paimage_tests8
```

结果：`[77/77]`，退出码 0。

在 Qt 6.8 MinGW Debug 运行时和 `QT_QPA_PLATFORM=offscreen` 下，按单测试进程逐项执行 CTest，共 34/34 通过：

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/paimage_tests8 -R '^<test-name>$' --output-on-failure
```

通过项包括 `physical_round_normalizer_test`、`paimage_host_output_test`、`ring_block_assembler_test`、`paimage_network_test`、`paimage_socket_short`、`paimage_discovery_socket_checks` 以及现有的 session、START fence、batch、trace、worker、protocol、conversion 和 looplog 回归。

Windows MinGW Debug：

```powershell
Set-Location MC_410T_MultiCard/delivery
cmd /c build_mingw_debug.cmd configure
cmd /c build_mingw_debug.cmd build
```

结果：configure 通过，`PAimageReceiverDiagnostics.exe` 链接和 Qt 部署通过，退出码 0。构建环境报告的 Vulkan headers、Qt translations catalog、DX compiler 警告不是本次源码构建失败。

一次性批量 CTest 在 Windows 进程复用场景曾出现 `0xc0000135` loader failure；同一测试单独运行及上述 34 次隔离 CTest invocation 全部通过，因此本报告采用可复现的隔离结果，不把该环境 launcher 问题记作测试断言失败。

## 修改文件

```text
MC_410T_MultiCard/delivery/include/MainWindow.h
MC_410T_MultiCard/delivery/include/NetworkController.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/Backend.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/HostOutput.h
MC_410T_MultiCard/delivery/include/PaimageAcquisition/PhysicalRoundNormalizer.h
MC_410T_MultiCard/delivery/include/RingBlockAssembler.h
MC_410T_MultiCard/delivery/src/MainWindow.cpp
MC_410T_MultiCard/delivery/src/NetworkController.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/Backend.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/HostOutput.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/PhysicalRoundNormalizer.cpp
MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp
MC_410T_MultiCard/delivery/tests/paimage_core/physical_round_normalizer_test.cpp
MC_410T_MultiCard/delivery/tests/paimage_host_output_test.cpp
MC_410T_MultiCard/delivery/tests/ring_block_assembler_test.cpp
```

## 网络权限与硬件边界

本次 socket 相关测试确实执行了真实的 Windows WinSock 本机 UDP loopback bind/send/receive，并由测试断言收发、session 和网络诊断结果；它不是只检查字符串或静态 fixture。但这类测试证明的是本机测试进程可用的回环路径，不等价于外部物理网卡、真实采集卡、ImagingSvc 或现场防火墙权限已验证。

首次运行弹出的 Windows 网络/安全权限对话框没有被点击允许。按照 computer-use 的安全约束，不能代替用户操作安全或隐私权限请求，因此没有把这部分外部权限写成通过，也没有宣称完成硬件验证。当前状态仍为 `HARDWARE_VALIDATION_PENDING`；不影响已通过的本机回环和软件 contract/regression 测试结论。

本任务没有修改 START fence、PREPARING/ARMED/RUNNING、UDP buffer、FPGA protocol、CUDA numerical reconstruction、binary save format 或笔记本 startup ingress loss。

最终推送后核对：

```text
git rev-parse HEAD == git rev-parse origin/codex/physical-round-normalizer-20260914-043019
```
