# Session B Review Addendum Execution Receipt — PAimage production 跳号统计修复

任务文档：`TASKS/SessionB审查追加_PAimage生产跳号统计修复_20260917.md`
（`origin/codex/task-docs`，commit `926fb21`）

## 1. Git identity

```text
branch             = codex/session-b-ui-observability-20260917
starting SHA       = 5f6d3ff10779cf93d0da9f2976120cac4fc7f8b9（已逐位核对 HEAD == 远端 == 起点）
fix code commit    = 93a2b9ce5cc40133767f6248900526a825e9b7ad
test-stability fix = 55898159668cc366dc7a1dd3f6767efa1adf01b7（仅 tests/CMakeLists.txt 资源锁，见 §5.8）
final remote HEAD  = 随最终执行报告带外上报（本 receipt 为文档 commit，避免自引用）
tracked git status = 代码 commit 时 clean；推送前再次 clean（见末节核验）
```

未新建实现分支；未把 main merge/rebase/cherry-pick 进本分支。

## 2. Root cause（确认）

`missingTriggerCount` 与完整 missing-trigger 的 `packetsDropped` 包当量原本只在
`DataProcessor::processInputBatch()` 的 legacy/test packet-assembly 路径累计
（Session B commit `917a719`，B1-B4 测试覆盖该路径）。

正式 PAimage production 数据路径为：

```text
NetworkController::start() -> startPaimage() -> paimage::Backend
  -> SocketReceiver -> SourceCore          （UDP ingress + 组装 + observation）
  -> HostOutput::card()/sync()             （Normalizer 分类/过滤）
  -> DataProcessor::deliverAssembled(...)  （直接交付已组装帧）
```

`HostOutput` 不把生产 packet 送入 `enqueuePacket()/processInputBatch()`，
因此现场的 `跳号数` 与完整 trigger 丢包当量在 production 路径下不增长。审查结论属实。

## 3. Final production owner（最终实现）

**SourceCore 拥有 gap 事实；NetworkControllerPaimage 拥有 CardStats 写入。**

- gap 计算点：`SourceCore::ingest()` 的新 assembly 激活块
  （`src/PaimageAcquisition/SourceCore.cpp`）。该点是 TriggerSwitch 路径与
  空 assembly 路径的唯一汇合点，每次激活恰好判定一次（exactly-once）。
- 锚点状态：`Assembly::gapAnchor / gapAnchorValid`
  （`include/PaimageAcquisition/SourceCore.h`），per-card，语义为
  "本卡最近一次 forward 接受的 trigger"。仅 forward progression
  （`int16_t(new-old) > 0`）移动锚点；`delta > 1` 时计 `delta - 1`。
  backstep/迟到 trigger（delta <= 0）不计数、不移动锚点；
  `RecentTrigger` reject 在激活点之前 return，天然不会重计。
- 事件载体：`paimage::Decision::TriggerGap`（enum 末尾追加，值 37；
  既有值被 trace 按 uint8 序列化，追加保证兼容）。Observation 携带
  card / 新 trigger / 激活 packet / count（缺失 trigger 数）/ firstIngressId，
  经 SocketReceiver 既有 observer lambda 写入 stage-2 trace 并转发
  `observationSink`。该事件纯 observability，不改变 admission/assembly/sync。
- CardStats 写入：`NetworkControllerPaimage.cpp::createPaimageBackend()` 的
  `observationSink` 新增 TriggerGap 分支：
  `missingTriggerCount += o.count`；
  `packetsDropped += o.count * expected`（`expected = m_config.packetsPerTrig()`，
  与 SourceCore 内部 expected_ 同源同值）。
- session 边界：锚点与 recent 窗口同生命周期——`clearAssembly(a, recent=true)`
  同时清除二者，该路径仅由 `prepareStart` / `completeStart(success)` 触发；
  `clearStartup`（startup idle 清理，recent=false）保留锚点与 recent，
  二者不变式一致。跨 session 不产生假跳号。
- double count 规避：`ingressSink` 保持只做原始 per-datagram 计数，不推导 gap；
  `TriggerGap` 只由 SourceCore 激活块发射一次，`observationSink` 是唯一消费者；
  DataProcessor legacy gap 逻辑所在路径 production 不经过（§2），两条实现
  不会处理同一 packet stream（源码证据：`HostOutput.cpp` 仅调
  `deliverAssembled`，`processInputBatch` 仅由 legacy enqueue 路径触达）。
- `triggersPartial` 语义未动（TriggerSwitch/Timeout 既有分支保持原样）；
  `runtimeIncomplete` 定义未动；UI 口径 `缺失=triggersPartial`、
  `跳号数=missingTriggerCount`、`丢包=packetsDropped` 保持。
- `runtimeStatsFields()` 的 `missingTriggerCount` JSON 映射为 Session B 既有
  实现，未改。
- 诊断工具 `tools/startup_diagnostics_analyze.py`：`DECISION_NAMES` 追加
  `37: "TriggerGap"`，并将其排除在 stage-2 reject 统计之外（与
  Complete/TriggerSwitch/Timeout 同类，属 observation 而非 reject）。

## 4. Changed files

fix code commit `93a2b9c`：

```text
MC_410T_MultiCard/delivery/include/PaimageAcquisition/SourceCore.h   (+4/-1)
MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp     (+12/-2)
MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp (+7)
MC_410T_MultiCard/delivery/tests/CMakeLists.txt                      (+7，注册新测试)
MC_410T_MultiCard/delivery/tests/paimage_production_gap_test.cpp     （新增）
MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py      (+2/-1)
```

test-stability commit `5589815`（仅测试基础设施，见 §5.8）：

```text
MC_410T_MultiCard/delivery/tests/CMakeLists.txt                      (+5，端口资源锁)
```

未触碰 UI/Ring/FileSaver/CUDA/Normalizer/FileSaver timeout 等 §11 禁止范围。

## 5. Exact tests / build（逐条真实命令与结果）

工作目录：`MC_410T_MultiCard/delivery`（worktree
`D:/ChatGPT/PAERealtimeImaging/_worktrees/session-b-ui-observability-20260917`）。
工具链：MinGW 13.1.0（Qt 6.8.0 自带）、CMake/Ninja（Qt Tools），
`build/tests_all_mingw_debug`（Session B 已配置的 Debug 测试目录；CMakeLists
变更由 Ninja 自动重配置）。

1. 测试构建（focal target）：
   `cmake --build build/tests_all_mingw_debug --target paimage_production_gap_test -j 8`
   → PASS（28 步，链接成功，exit 0）
2. Focal production-gap 测试（B-ADD-1..6）：
   `./build/tests_all_mingw_debug/paimage_production_gap_test.exe`
   → PASS，exit 0。输出：
   - `PASS B-ADD-1 production full gap: T100->T104 missing=3 dropped=3 partial=0`
   - `PASS B-ADD-2 production partial+full gap: partial=1 missing=3 dropped=21`
   - `PASS B-ADD-3 production adjacent: T200->T201 missing=0 dropped=0`
   - `PASS B-ADD-4 production wrap forward: T65534->T1 missing=2 dropped=2`
   - `PASS B-ADD-5 production backstep/stale: missing stays 4, anchor never retreated`
   - `PASS B-ADD-6 production session reset: T100 then T120 after restart, missing=0`
3. 全量测试构建：`cmake --build build/tests_all_mingw_debug -j 8` → PASS（84 步）
4. 完整 CTest：`ctest --output-on-failure -j 4` → **42/42 PASS**（38.31 s；
   含 Session B 既有 B1-B4 `data_processor_batch_test`、
   `card_status_formatting_test`、`round_policy_settings_test`、
   `network_diagnostics_test`、`physical_round_normalizer_test`、
   `paimage_host_output_test` 与新增 `paimage_production_gap_test`）
5. Session B 点名回归子集：
   `ctest --output-on-failure -R "data_processor_batch_test|card_status_formatting_test|round_policy_settings_test|network_diagnostics_test|physical_round_normalizer_test|paimage_host_output_test|paimage_production_gap_test"`
   → 7/7 PASS
6. 代码 commit（93a2b9c）后完整 CTest 复跑：`ctest --output-on-failure -j 4`
   → 42/42 PASS（结果见本文件提交前工作区 == 该 commit 树）
7. Windows 完整构建（在 fix code commit 93a2b9c 上）：
   `cmd /c build_mingw_debug.cmd` → **exit 0**（configure preset `mingw-debug`
   + build preset `mingw-debug-build` + windeployqt，脚本自检四个 exe 齐备）。
   该 commit 之后唯一的代码侧改动是 tests/CMakeLists.txt 的测试注册属性
   （5589815），不属于 app 构建输入，故 Windows 构建结果对最终 HEAD 仍然有效。
8. 端口竞争修复与复跑（test-stability commit `5589815`）：
   `ctest -j 4` 第二轮曾出现 `data_processor_batch_test` 偶发
   "FAIL receiver start"。定位为 baseline 预存竞争：ctest #15
   `session_boundary_receiver_test` 与 #16 `data_processor_batch_test` 均经
   MultiPortReceiver 绑定固定端口 INADDR_ANY:8001，且二者无串行化属性，
   `-j 4` 下可并发撞端口。修复：在 tests/CMakeLists.txt 为二者设置共享
   `RESOURCE_LOCK paimage_base_port_8001`（仅测试调度属性，不改任何源码）。
   修复后 `ctest -j 4` **连续两轮 42/42 PASS**（23.13 s / 23.11 s）。

测试 seam 说明：`paimage_production_gap_test` 复用 `paimage_network_test` 的
production 链路（同一组生产源码 + `paimage_socket`），loopback 上单张假卡
（127.0.0.2:8080 收 CONFIG/START/STOP 线包并回 60 字节 ACK），数据包发往
127.0.0.1:8001，经真实 `SocketReceiver -> SourceCore -> observationSink ->
CardStats` 后用 `NetworkController::getAllCardStats()` 断言。无任何 fake gap
计算器；gap 判定只存在于生产 `SourceCore::ingest()`。

既有环境事实（非本次改动引入）：`network_diagnostics_test` 在 ctest 中以
`--legacy-control-only` 注册（CMakeLists.txt:119），其完整模式的
measurement-boundary/reception 用例在当前环境本就不运行；production 准入/导出
由 `paimage_network_test` 与本次新增的 `paimage_production_gap_test` 覆盖。
该行为在 baseline `5f6d3ff` 上实测一致。

## 6. Dependencies（按 BUILD_STANDARD 适用范围）

本 addendum 不修改 CUDA core，未重新生成 CUDA runtime；下列依赖与 Session B
receipt 记录一致、本轮未变（已实测哈希，来自 Session A 交付链）：

```text
ring_recon_cuda.dll        BF40472D5A46363A35DD1084A01A8C15F13EF203F3ED5B4BB5EDF767EEEC14B5
libring_recon_cuda.dll.a   94ABAB973582D06DEBF3831366BAEC00458EA3277F5F3B852CB1FA24A3C3F5
cudart64_12.dll            C2C9A9C22A9BCBA90E261825968836787B331038047A26770CFFB7A583C28344
pa_recon_core.dll          C1A37E73147C1B8250F96CCDC1FCFC08CA28CECBF1CB3C046D4AB426F2EA0893
libzmq-v141-mt-4_3_5.dll   37610023D91951BC4177DB1F96B54B28911976AC70B221A852887709A02A9AD3
cufft64_12.dll             2480D8AB849D7E9A375275F6C0278B8764C14AC0C1A3BDACAF256AE4A93C5590
```

## 7. Acceptance 对照（§20）

1. production source path 识别 forward full gap —— SourceCore 激活块，PASS（B-ADD-1/2/4）
2. T100->T104 精确 `missingTriggerCount += 3` —— B-ADD-1 PASS
3. 同场景 `packetsDropped += 3 * expectedPackets` —— B-ADD-1（3*1）/B-ADD-2（3*6）PASS
4. partial + full gap 并存且不混淆 —— B-ADD-2（partial=1, missing=3, dropped=21）PASS
5. adjacent 不计 —— B-ADD-3 PASS
6. uint16 wrap forward —— B-ADD-4（T65534->T1 = +2）PASS
7. late/backstep/recent 不制造假跳号 —— B-ADD-5（含锚点不回退证明）PASS
8. session reset 锚点隔离 —— B-ADD-6 PASS
9. exactly once —— 激活块唯一发射点 + RecentTrigger 前置 return；B-ADD-1..6 计数精确
10. admission/assembly/sync/Normalizer/Ring/CUDA/FileSaver 行为未改 —— diff 仅限 §4 文件；
    `paimage_network_test`（真实准入/存储/停启）PASS
11. DataProcessor B1-B4 继续 PASS —— `data_processor_batch_test` PASS
12. 新 production-seam 测试 PASS —— 见 §5.2
13. 完整 CTest PASS —— 42/42
14. Windows full build PASS —— exit 0
15. receipt + HANDOFF 更新随文档 commit 推送
16. local HEAD == remote HEAD —— 推送后 `git ls-remote` 核验（见最终报告）
17. hardware validation 保持 PENDING —— 本任务全部为 loopback/自动化验证

## 8. Validation boundary

```text
SESSION_B_SOFTWARE_IMPLEMENTATION = PASS
SESSION_B_PAIMAGE_PRODUCTION_GAP_ACCOUNTING = PASS
SESSION_B_AUTOMATED_TESTS = PASS (ctest 42/42)
SESSION_B_WINDOWS_BUILD = PASS
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS = NOT PROVEN BY THIS TASK
```

本轮全部为软件侧 loopback/单元级确定性验证；不得解释为真实 FPGA/NIC/LabVIEW
实机跳号行为已验证。
