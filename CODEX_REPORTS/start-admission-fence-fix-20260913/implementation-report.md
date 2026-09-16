# START Fence 语义修复执行报告

## 任务与 Git 基线

- 初始任务文档：`TASKS/PAimage_START_admission语义修复_20260913-003112.md`
- 追加整改文档：`TASKS/PAimage_START_admission验收整改追加_20260913-021700.md`
- 追加整改开始前远端 HEAD：`30987c183b7f7cb172ae0137d7132dea884c812d`
- `origin/main` 基线：`f326056ee99e5f9c135635d7e97fe2027ee50fbc`
- 实现分支：`codex/start-admission-fence-fix-20260913-003112`
- 本次整改实现提交：`2903a2b4851f9f668f202dfff77ec88678441e22`
- 本报告路径：`CODEX_REPORTS/start-admission-fence-fix-20260913/implementation-report.md`

追加整改沿用原分支和起始远端 HEAD；未新建分支、rebase、merge validation、修改 main 或 force-push。

## 本次整改

### A. `completeStart(false)` 失败闭环

`SocketReceiver::completeStart` 现在始终把 `ok=false` 视为失败：只有 `ok=true` 且没有收到 per-card callback 时，才保留无 callback 的兼容标记；最终 `fenceOk` 同时要求 `ok`、未失败以及所有卡为 `StartSent`。因此直接回归中的 `prepareStart(sessionA); completeStart(false)` 返回 false，SourceCore 保持 disabled，后续数据不会产生 frame/sync/release；`completeStart(true)` 的无 callback 兼容路径仍可恢复。

### B. 诊断分析器采用 decision-first 语义

`startup_diagnostics_analyze.py` 对 `(session, correlation, card, trigger, packet, ingressId)` 做精确关联，并先解释 stage2 decision：同一 ingress 出现 `StartFenceHeld` 即表示合法 pending window，即使 raw ingress 时间早于 START stage6 result observation time；`StartFencePreStartDiscard` 明确记为 stale，直接 `Disabled` 记为 gate-drop。只有没有可解释证据时才记为 missing/inconclusive。`startSendNs` 明确是发送结果观察时间，不再被误用成唯一 admission lower boundary。

### C. 验收回归接入

- 新增无 callback 的 false-start 回归：确认失败闭环和下一次兼容启动恢复。
- 新增 hold overflow 回归：9000 个 datagram 触发生产 hold 上限，确认 overflow、failed discard、无 release/输出及恢复。
- 扩充真实 UDP race trace：60 个成功 session（whole/prefix/normal 各 20），包含 pre-start stale、wrap-near trigger、失败 session 和恢复 session；记录 raw/held/released、Disabled、frame、sync、cross-session leak、double release。
- 新增自动化 CTest `paimage_start_race_analyzer`，依赖 `paimage_start_race`，直接分析最新真实 trace 并把数值证据写入 `result.json`。
- 失败场景在注入 START failure 前先确认至少一笔 pre-trigger ingress 已进入 fence，随后等待失败 ingress drain，避免测试 harness 因线程调度互等。

## 验收数值证据

以下为最后一次通过 CTest 生成的真实 trace 结果；临时 build/artifact 目录已在验证后清理。

| 指标 | 结果 |
| --- | --- |
| session 总数 / 成功 session | 61 / 60 |
| whole / prefix / normal | 20 / 20 / 20 |
| wrap-near session | 6 |
| 成功 session raw / held / released | 3841 / 880 / 880 |
| post-boundary Disabled | 0 |
| complete / partial frame | 480 / 0 |
| sync frame | 120 |
| cross-session frame leak / double release | 0 / 0 |
| hard socket error / emulator send error | 0 / 0 |
| trace queue dropped / trace incomplete | 0 / false |

失败 session 最新值为：`startReturnedFalse=true`、`fenceReturnedFalse=true`、`heldCount=10`、`failedDiscardCount=11`、`releasedCount=0`、`cardFrameCount=0`、`syncFrameCount=0`、`recoverySessionPassed=true`。失败期间的 held/failed-discard 数量随 in-flight UDP 到达顺序变化，但没有 release 或输出。

真实 trace analyzer 的结果为：`successSessionCount=60`、`cleanSessionCount=61`（含 recovery session）、`failedSessionCount=1`、`inconclusiveSuccessSessions=0`、`gateDropSuccessSessions=0`、`missingStage2JoinCount=0`、`rawIngressCount=3937`、`heldIngressCount=890`、`releasedIngressCount=880`、`traceIncomplete=false`。

合成 analyzer fixture 另外验证了：clean case 的 `held=1`、`released=1`、`missing=0`；pre-start stale case 的 `preStartDiscard=1` 且 `missing=0`。overflow 回归输出 `overflowCount=1`、`failedDiscardCount=9000`、`releasedCount=0`、`cardFrameCount=0`、`syncFrameCount=0`、`recoveryPassed=true`。无 callback false-start 回归输出 `completeStartFalseReturnedFalse=true`、`sourceRemainedDisabled=true`、`releasedCount=0`、`cardFrameCount=0`、`syncFrameCount=0`、`nextCompatibilityStartPassed=true`。

## 验证命令与结果

- Python `py_compile`：通过 analyzer、synthetic fixture 和 CTest analyzer 脚本语法检查。
- `startup_diagnostics_analyze_test.py`：通过 clean、gate-drop、failed、incomplete、session-correlation、pre-start fixture。
- tests build：`cmake --build <tests-build> --parallel 2`，通过。
- 任务相关 CTest：`paimage_network_test`、`paimage_start_race`、`paimage_start_fence_regression`、`paimage_start_overflow`、`startup_diagnostics_analyze_test`、`paimage_start_race_analyzer`，6/6 通过。
- `paimage_core` CTest：11/11 通过。
- `paimage_start_race` 连续稳定性复测：5/5 通过。
- 生产 MinGW Debug 构建：主程序、`ImagingSvc`、Ring/CUDA 依赖部署及 Qt 部署步骤通过（57/57）。构建所需的 `cufft64_12.dll` 仅从原工作区临时复制到干净副本，验证后已删除，未进入提交。

历史全套 Qt CTest 调度中有既有目标报告 Windows `0xc0000135`，而相关目标逐项执行可通过；本次门禁采用任务相关目标和 `paimage_core` 全集。未进行真实 FPGA/NIC 硬件采集或 system capture 验证。

## Git 交付凭据

实现提交 `2903a2b4851f9f668f202dfff77ec88678441e22` 相对 `origin/main` 的变更统计：

```text
15 files changed, 1480 insertions(+), 26 deletions(-)
```

变更路径：

```text
A  CODEX_REPORTS/start-admission-fence-fix-20260913/implementation-report.md
M  MC_410T_MultiCard/delivery/include/PaimageAcquisition/ControlSocket.h
M  MC_410T_MultiCard/delivery/include/PaimageAcquisition/SocketReceiver.h
M  MC_410T_MultiCard/delivery/include/PaimageAcquisition/SourceCore.h
M  MC_410T_MultiCard/delivery/src/PaimageAcquisition/Backend.cpp
M  MC_410T_MultiCard/delivery/src/PaimageAcquisition/ControlSocket.cpp
M  MC_410T_MultiCard/delivery/src/PaimageAcquisition/SocketReceiver.cpp
M  MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp
M  MC_410T_MultiCard/delivery/tests/CMakeLists.txt
A  MC_410T_MultiCard/delivery/tests/paimage_start_fence_regression_test.cpp
A  MC_410T_MultiCard/delivery/tests/paimage_start_overflow_test.cpp
A  MC_410T_MultiCard/delivery/tests/paimage_start_race_analyzer_test.py
A  MC_410T_MultiCard/delivery/tests/paimage_start_race_test.cpp
A  MC_410T_MultiCard/delivery/tests/startup_diagnostics_analyze_test.py
M  MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py
```

## 最终验收收口

本轮 review starting HEAD 为 `1e53769729d0d76232c86f886e3d3e87a8af8c74`。本轮唯一源码/test 提交为 `ffd9eae66502d87170bb0c83721386516c6fe236`，仅在 `paimage_start_race_test.cpp` 中加入失败 session 硬门禁；production source 与 analyzer 在本轮未修改。report-only 提交与最终远端 HEAD 按照最终交付消息单独给出，避免报告文件自引用自身 SHA。

### Injected START failure hard gates

`runFailureAndRecovery()` 在 recovery session 之前、且在 `failed session ingress drained` 等待完成后，直接 `require()` 以下条件；因此这些条件由 test executable 强制执行，而不是只由报告人工查看：

```text
Injected START failure conditions are enforced by test assertions: YES
startReturnedFalse        = true
fenceReturnedFalse        = true
heldCount                 > 0
failedDiscardCount        > 0
releasedCount             = 0
cardFrameCount            = 0
syncFrameCount            = 0
recoverySessionPassed     = true
```

本轮最新 race artifact `run-138524000198` 的实际失败 session 数值为：`heldCount=8`、`failedDiscardCount=11`、`releasedCount=0`、`cardFrameCount=0`、`syncFrameCount=0`、`recoverySessionPassed=true`。

### 本轮验证

- `paimage_start_race` + `paimage_start_race_analyzer`：2/2 通过；analyzer 读取本轮新生成 artifact。
- `paimage_start_fence_regression`、`paimage_start_overflow`、`startup_diagnostics_analyze_test`、`paimage_network_test`：4/4 通过。
- `paimage_core`：11/11 通过。
- Python `py_compile` 与 synthetic analyzer fixture：通过。
- tests full relevant build：通过；`paimage_start_race_test` 已重新编译。
- `git diff 1e53769729d0d76232c86f886e3d3e87a8af8c74..ffd9eae66502d87170bb0c83721386516c6fe236 -- MC_410T_MultiCard/delivery/src MC_410T_MultiCard/delivery/include`：空；本轮 production source unchanged。

### 本轮更新时的完整 Git receipt

以下为源码/test 提交 `ffd9eae66502d87170bb0c83721386516c6fe236` 相对 `origin/main` 的完整统计；report-only commit 只会改变本报告文件：

```text
15 files changed, 1548 insertions(+), 26 deletions(-)
```

```text
A  CODEX_REPORTS/start-admission-fence-fix-20260913/implementation-report.md
M  MC_410T_MultiCard/delivery/include/PaimageAcquisition/ControlSocket.h
M  MC_410T_MultiCard/delivery/include/PaimageAcquisition/SocketReceiver.h
M  MC_410T_MultiCard/delivery/include/PaimageAcquisition/SourceCore.h
M  MC_410T_MultiCard/delivery/src/PaimageAcquisition/Backend.cpp
M  MC_410T_MultiCard/delivery/src/PaimageAcquisition/ControlSocket.cpp
M  MC_410T_MultiCard/delivery/src/PaimageAcquisition/SocketReceiver.cpp
M  MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp
M  MC_410T_MultiCard/delivery/tests/CMakeLists.txt
A  MC_410T_MultiCard/delivery/tests/paimage_start_fence_regression_test.cpp
A  MC_410T_MultiCard/delivery/tests/paimage_start_overflow_test.cpp
A  MC_410T_MultiCard/delivery/tests/paimage_start_race_analyzer_test.py
A  MC_410T_MultiCard/delivery/tests/paimage_start_race_test.cpp
A  MC_410T_MultiCard/delivery/tests/startup_diagnostics_analyze_test.py
M  MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py
```

原始脏工作区未修改；本分支未修改 main、未 rebase/squash/force，最终 local/remote SHA、working tree clean 和 report-only commit SHA 由最终交付消息给出。
