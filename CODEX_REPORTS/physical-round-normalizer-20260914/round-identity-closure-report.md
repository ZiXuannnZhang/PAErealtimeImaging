# 物理轮次 RoundIdentity 闭环与 Ring 跨轮污染整改追加 — 执行报告

## 结论

```text
CODE_IMPLEMENTED
AUTOMATED_TESTS_PASS
WINDOWS_BUILD_PASS
HARDWARE_VALIDATION_PENDING
```

已按任务 `TASKS/物理轮次RoundIdentity闭环与Ring跨轮污染整改追加_20260916.md` 完成物理轮次身份从生产者、服务、Ring assembler 到 presentation 的闭环。Ring 新身份到达时形成真正的跨轮硬屏障：旧轮残留不会被新轮补齐，旧轮迟到数据不会回滚当前轮；CountBoundary 观察者不再直接清空 assembler，也不再用 trigger 序号猜测物理轮次。

未修改 `PhysicalRoundNormalizer` 的分类/状态机语义、`SourceCore` partial 语义、UDP/wire 协议和既有 `submit_index` stale cutoff。真实采集卡、FPGA、现场 NIC/GPU 和长时间负载仍需硬件复核。

## 任务身份与提交

```text
TASK_DOC              TASKS/物理轮次RoundIdentity闭环与Ring跨轮污染整改追加_20260916.md
IMPLEMENTATION_BRANCH codex/physical-round-normalizer-20260914-043019
CONTINUE_FROM_SHA     6f5f56372e5180f29fe2ce679c494075161fe88f
SOURCE_FIX_SHA        76e773a824c1a82484b7214410cc63176d8494a2
RECEIPT_SHA           33f21995619a1faa97fad7e5d66d8e1ed37f0147
```

实现提交的父提交与 `CONTINUE_FROM_SHA` 一致；没有 reset、rebase 或 force push。主工作区原有用户改动未触碰。

## Implementation

### RoundIdentity 数据模型和传输

`RoundIdentity := (measurementSession, roundGeneration)`，两个字段均为 `uint64_t`。`measurementSession == 0` 保留为无效身份，generation 0 允许作为测量会话的初始物理轮。`RoundIdentity` 提供相等和按 session/generation 的稳定排序，成为 Ring block、snapshot 和 presentation 的唯一物理轮归属键。

`RingRoundIdentity` 使用 JSON 十进制字符串无损承载两个 uint64 字段：

```text
measurement_session
round_generation
```

解析要求两个字段都存在、均为非空十进制字符串且 session 非零；JSON number、缺失字段、非法字符、溢出和零 session 均 fail closed。`submit_index` 仍作为独立的 producer snapshot 序列存在。

生产路径的消息闭环为：

```text
ring_block_ready
  producer -> ImagingSvc: seq, submit_index, measurement_session, round_generation
ring_snapshot_ready
  ImagingSvc -> ImagingController: 原样回显同一 RoundIdentity，并保留 submit_index
```

服务端只解析、校验和回显输入身份，不生成 service counter/seq 作为轮次，也不使用 latest/current identity fallback。缺失或无效身份被拒绝并低频记录 `round_identity_rejected`。Controller、worker request、snapshot signal、MainWindow callback 均携带同一身份。

### Ring assembler 硬屏障

`RingBlockAssembler` 的每条 channel line 都带有 `RoundIdentity`，block callback 也带有 identity。assembler 维护 active/minimum/block identity：

```text
first valid identity  -> establish active
same identity         -> normal assembly
greater identity      -> clear pending/block/phase, then accept new line
older identity        -> stale drop, never rollback
```

当新身份到达时，会在接受该身份第一条 line 之前清理旧 pending trigger、旧 block、旧 channel phase 和全局 angle/wavelength phase。若清理时存在残留，记录 active/incoming identity、clean/residual、pending/block 丢弃计数；同一 session 的 generation 跳跃记录 gap。identity 缺失/无效、低于 measurement-session floor 或 block 内 identity 不一致均 fail closed。pending map 仍有固定上限并记录 eviction；热路径只做 O(1) identity 比较和有界状态更新，没有新增 receiver packet bookkeeping 或每触发 I/O。

CountBoundary observer 只负责发布对应的 save/presentation transition，不调用生产 assembler reset。保留的 `completeLogicalRound()` 只作为兼容/测试 seam：它只在状态干净时清 phase，不能清掉含旧轮残留的生产状态；实际跨轮 barrier 由 incoming RoundIdentity 驱动。

### Presentation 精确匹配

`RingRoundPresentationState` 以精确旧 `RoundIdentity` 为 key 保存多个 pending transition，保存 old/new directory 和 autosave commit result，映射有界（最多 128 条）。snapshot 完成时只查找其自身旧轮 identity：

- 精确命中时应用对应 target，必要时使用该 transition 的 old directory 保存旧 PNG；
- 迟到旧 snapshot 可完成自己的旧目录，但不能回滚 current target；
- 重复 transition/snapshot 为幂等 no-op；
- 缺失身份、未知 identity 和无法匹配的 transition fail closed；
- 新旧 pending transition 可并存，按 identity 精确消费，不依赖“最新 boundary”。

`submit_index` 仍只负责 snapshot stale/duplicate admission 和 TimeoutBoundary cutoff；RoundIdentity 独立负责物理轮次 presentation 路由，二者没有互相替代。

### Lifecycle 与诊断

presentation 映射及 admission 状态在 measurement session begin、service stop/restart、listener rebuild、Ring 配置 restart、autosave reconfigure/disable 等生命周期点清理；assembler 同时更新 session floor，避免旧 session 回流。Ring UI 的 admitted submit index 保持有界，TimeoutBoundary 保留既有 stale cutoff 时序。

低频诊断覆盖 `ring_round_transition`、`ring_presentation_transition`、`round_identity_rejected` 以及 invalid/stale/duplicate 等 action，字段包含 active/incoming identity、transition kind、pending/block 计数、丢弃计数、snapshot identity、submit index 和 presentation action。没有增加 per-trigger 日志。

## Tests

### R1–R14 专项覆盖

新增/扩展测试覆盖如下：

```text
R1  旧轮 partial residual 后，新 identity 只能形成新值 block，旧残留不泄漏
R2  clean CountBoundary observer 不丢迟到旧轮 final sync
R3  stale old group/drop 不进入 block
R4  block identity invariant、session floor、generation gap fail closed/记录
R5  producer -> service -> snapshot identity 原样 echo；missing/invalid reject
R6  多个 pending presentation transition 按 exact old identity 匹配
R7  反向/迟到旧 snapshot 不回滚当前 presentation target，并使用旧目录
R8  duplicate transition/snapshot 幂等
R9  TimeoutBoundary、submit_index cutoff 与 RoundIdentity stale 责任分离
R10 measurement session restart 后旧 session 数据丢弃，新 session 可正常开始
R11 sync/Ring drop residual 的 identity hard barrier
R12 既有 save binding、FileSaver、HostOutput 回归
R13 PhysicalRoundNormalizer 原有 T1-T17 回归
R14 uint64 十进制序列化、number/missing reject、echo 与 submit_index 共存
```

相关新增/扩展目标包括 `ring_block_assembler_test`、`ring_round_identity_test` 和 `ring_shm_observability_test`；既有 `auto_save_round_coordinator_test`、`count_boundary_save_binding_test`、`filesaver_round_boundary_test`、`paimage_host_output_test` 及 `physical_round_normalizer_test` 均通过。

### 全量 CTest

```powershell
$ctestPath = 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/ctest.exe'
$env:Path = 'D:/Qt/Qt6.8.0/6.8.0/mingw_64/bin;D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin;C:/Windows/System32;C:/Windows;' + $env:Path
& $ctestPath --test-dir 'MC_410T_MultiCard/delivery/build/paimage_tests8' -R '^<test-name>$' --output-on-failure -j1
```

最终 `ctest -N` 注册 39 个目标；逐项隔离执行结果：

```text
PASSED=39 FAILED=0
```

`git diff --check` 通过；Git 仅提示工作树 LF/CRLF 转换警告，没有 whitespace error。

## Build

工具链：Qt 6.8.0 `mingw_64`、MinGW 13.1、CMake、Ninja。

测试工程最终 reconfigure/build：

```powershell
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe' -S tests -B build/paimage_tests8 -G Ninja
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe' --build build/paimage_tests8 --parallel 4
```

结果：PASS。配置阶段有 `Could NOT find WrapVulkanHeaders (missing: Vulkan_INCLUDE_DIR)` 提示，但 `Configuring done`、`Generating done` 和完整构建均成功。

Windows Debug 主工程：

```powershell
& 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe' --build build/mingw_debug --parallel 4
```

结果：PASS；PAimage receiver、ImagingSvc 和 `ring_svc_selftest` 均完成编译/链接。windeployqt 的 Vulkan include、`catalogs.json`、`dxcompiler.dll/dxil.dll` 和 runtime path 提示属于本机部署环境警告，不是编译/链接失败。

`ring_svc_selftest` 已构建但未执行：工作区没有可用的 `11.dat`/`14.dat` 输入文件，且当前环境没有真实采集硬件/GPU 运行条件。该项保留为 `HARDWARE_VALIDATION_PENDING`，没有伪报 PASS。

## Changed files

```text
MC_410T_MultiCard/delivery/CMakeLists.txt
MC_410T_MultiCard/delivery/include/DataTypes.h
MC_410T_MultiCard/delivery/include/ImagingController.h
MC_410T_MultiCard/delivery/include/MainWindow.h
MC_410T_MultiCard/delivery/include/RingBlockAssembler.h
MC_410T_MultiCard/delivery/include/RingRoundIdentity.h
MC_410T_MultiCard/delivery/include/RingRoundPresentation.h
MC_410T_MultiCard/delivery/include/RingRoundUiState.h
MC_410T_MultiCard/delivery/include/RingShmObservability.h
MC_410T_MultiCard/delivery/include/RoundIdentity.h
MC_410T_MultiCard/delivery/src/ImagingController.cpp
MC_410T_MultiCard/delivery/src/ImagingSvc/ImagingSvc.cpp
MC_410T_MultiCard/delivery/src/ImagingSvc/ImagingSvc.h
MC_410T_MultiCard/delivery/src/MainWindow.cpp
MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp
MC_410T_MultiCard/delivery/src/RingReconCuda/ring_svc_selftest.cpp
MC_410T_MultiCard/delivery/src/RingRoundPresentation.cpp
MC_410T_MultiCard/delivery/src/RingRoundUiState.cpp
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tests/ring_block_assembler_test.cpp
MC_410T_MultiCard/delivery/tests/ring_round_identity_test.cpp
MC_410T_MultiCard/delivery/tests/ring_shm_observability_test.cpp
```

源码和测试已在 `SOURCE_FIX_SHA` 中单独提交；本报告仅在后续 report-only 提交中加入，未混入生产代码或测试变更。

## Known limitations / hardware

```text
HARDWARE_VALIDATION_PENDING
```

本次验证覆盖 Windows 本机 Ring assembler、identity codec、service/controller message path、presentation/UI admission、save binding、normalizer 回归和 Debug 构建；未替代真实 FPGA/NIC 多卡迟到与丢包时序、CUDA snapshot reset 竞态、现场磁盘/权限/防火墙和长时间压力验证。

## Local / remote HEAD

```text
LOCAL_HEAD_AT_SOURCE_COMMIT 76e773a824c1a82484b7214410cc63176d8494a2
LOCAL_HEAD_AT_REPORT_RECEIPT 33f21995619a1faa97fad7e5d66d8e1ed37f0147
REMOTE_HEAD_AT_REPORT_RECEIPT 33f21995619a1faa97fad7e5d66d8e1ed37f0147
```

`RECEIPT_SHA` 按项目约定指向首个 report-only 提交；随后仅为回填该 SHA 产生的修正提交仍只修改本报告，最终分支 HEAD 以最后一次 push 后的远端校验为准。
