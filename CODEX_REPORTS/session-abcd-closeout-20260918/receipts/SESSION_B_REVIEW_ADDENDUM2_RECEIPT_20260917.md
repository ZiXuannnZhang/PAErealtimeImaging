# Session B Second Review Addendum Execution Receipt — 同 Session triggerSeq 复位恢复

任务文档：`TASKS/SessionB二次审查追加_同Session触发序号复位恢复_20260917-211159.md`
（`origin/codex/task-docs`，commit `9fbae68`）

## 1. Git identity

```text
branch                  = codex/session-b-ui-observability-20260917
starting SHA            = 2566a028a590ec2e099a12dd00977f704c5e38e0
                          （执行前已核对 HEAD == 远端 == 起点，逐位一致）
reset-recovery code commit = 35953ee89c06c193667766468f7f47e6796288e3
final remote HEAD       = 随最终执行报告带外上报（本 receipt 为文档 commit，避免自引用）
tracked git status      = 代码 commit 时 clean；推送前再次 clean（见末节核验）
local/remote HEAD       = 推送后 git ls-remote 核验，结果见最终执行报告
```

未新建实现分支；未把 main merge/rebase/cherry-pick 进本分支。

## 2. Root cause

第一次 review addendum 建立的 production gap anchor
（`SourceCore::ingest()` fresh-assembly 激活点，`gapAnchor/gapAnchorValid`）
对 `delta <= 0`（signed uint16 wrap-aware 差值）一律不移动：

- 小幅 backstep / 迟到 trigger：不计 gap、不移动锚点 —— 这是**正确**行为，
  本 addendum 保持不变；
- 但**同一 measurement session 内大幅 triggerSeq 回退/源计数器复位**时，
  锚点永远停留在旧高值，后续所有 trigger 相对旧锚点均为 backward，
  `missingTriggerCount` 与 full-trigger `packetsDropped` 当量长期失明。

实例：anchor=T1000，源复位到 T10 再 T14。旧逻辑下 T10/T14 均不计 gap，
复位后真实缺失的 T11/T12/T13 无法统计。

## 3. Final semantics（最终实现与判断顺序）

实现位置：`SourceCore::ingest()` 的 fresh-assembly 激活块
（`MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp`），
与第一次 addendum 同一 exactly-once 发射点。

阈值常量：`SourceCore::kTriggerResetBackJumpThreshold = 256`
（`include/PaimageAcquisition/SourceCore.h`），与 DataProcessor 冻结语义
`DataProcessor::kTriggerResetBackJumpThreshold`（`include/DataProcessor.h:223`）
同值同语义，注释明示对齐；未在 DataProcessor 与 SourceCore 间建立新架构依赖。

判断顺序（先 signed forward/wrap，再判 direct large back-jump，避免把正常
wrap forward 误判为 reset）：

```cpp
const auto delta=static_cast<std::int16_t>(trigger-a.gapAnchor);
if(delta>0){                                 // forward / uint16 wrap forward
    if(delta>1)event(Decision::TriggerGap,...,std::uint32_t(delta-1),...);
    a.gapAnchor=trigger;
}else if(static_cast<std::int32_t>(a.gapAnchor)-static_cast<std::int32_t>(trigger)
         >=kTriggerResetBackJumpThreshold){  // 直接回退 >= 256
    a.gapAnchor=trigger;                     // reset recovery：重建锚点，不计 gap
}                                            // 其余小幅 backstep：不计数、不动锚点
```

Observable contract：

- T100→T104：`missingTriggerCount += 3`、`packetsDropped += 3*expectedPackets`（不变）；
- T65534→T1：signed delta = +3，仍按 forward 记 +2，不误判 reset；
- anchor T105→T102：backJump 3 < 256，不计 gap、锚点保持 T105；
- anchor T1000→T10：backJump 990 >= 256，reset transition 记 0 gap / 0 丢包当量，
  新锚点 T10；随后 T10→T14 记 +3；
- 阈值边界：backJump 255 不重建锚点；backJump 256 重建锚点；
- measurement session reset（`prepareStart/completeStart(success)` 清锚点）保持隔离。

未改动（§8/§16 边界）：packet admission、`RecentTrigger` reject、Duplicate /
OffsetOutside、TriggerSwitch/Timeout 组装、pending sync、startup buffering、
SourceCore complete/partial delivery、PhysicalRoundNormalizer、
`triggersPartial` / `runtimeIncomplete` 定义、UI 口径、Ring/FileSaver/CUDA。
`TriggerGap` 事件仍只由该激活块发射一次，`observationSink` 仍是唯一
CardStats 消费者；reset transition 不发射 `TriggerGap`，天然无双计。

## 4. Changed files（reset-recovery code commit `35953ee`）

```text
MC_410T_MultiCard/delivery/include/PaimageAcquisition/SourceCore.h   (+8/-2)
MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp     (+13/-4)
MC_410T_MultiCard/delivery/tests/paimage_production_gap_test.cpp     (+67/-4)
```

生产代码仅限 §17 预期的 SourceCore.h/.cpp，无范围漂移；文档为本 receipt 与
`HANDOFF_SESSION_B_20260917.md`（Session C 基线与测试计数收口）。

## 5. Exact tests / build（逐条真实命令与结果）

工作目录：`MC_410T_MultiCard/delivery`（worktree
`D:/ChatGPT/PAERealtimeImaging/_worktrees/session-b-ui-observability-20260917`）。
工具链：MinGW 13.1.0 + CMake/Ninja（Qt 6.8.0 Tools），测试目录
`build/tests_all_mingw_debug`。

1. Focal 测试构建：
   `cmake --build build/tests_all_mingw_debug --target paimage_production_gap_test -j 8`
   → PASS（链接成功，exit 0）
2. Focal production-gap 测试（B-ADD-1..8）：
   `./build/tests_all_mingw_debug/paimage_production_gap_test.exe` → PASS，exit 0：
   - `PASS B-ADD-1 production full gap: T100->T104 missing=3 dropped=3 partial=0`
   - `PASS B-ADD-2 production partial+full gap: partial=1 missing=3 dropped=21`
   - `PASS B-ADD-3 production adjacent: T200->T201 missing=0 dropped=0`
   - `PASS B-ADD-4 production wrap forward: T65534->T1 missing=2 dropped=2`
   - `PASS B-ADD-5 production backstep/stale: missing stays 4, anchor never retreated`
   - `PASS B-ADD-6 production session reset: T100 then T120 after restart, missing=0`
   - `PASS B-ADD-7 production same-session reset: T1000->T10 no gap, T10->T14 missing=3 dropped=3`
   - `PASS B-ADD-8A production threshold 255: T400->T145 backstep, T401 adjacent, missing=0`
   - `PASS B-ADD-8B production threshold 256: T400->T144 reset, T144->T148 missing=3 dropped=3`
3. 全量测试构建：`cmake --build build/tests_all_mingw_debug -j 8` → PASS（57 步）
4. 完整 CTest（最终代码 commit 上）：
   `ctest --output-on-failure -j 4` → **42/42 PASS**（27.03 s）。
   既有的 `RESOURCE_LOCK paimage_base_port_8001` 测试稳定性修复保留未删（§18）。
5. Session B 点名回归子集：
   `ctest --output-on-failure -R "data_processor_batch_test|card_status_formatting_test|round_policy_settings_test|network_diagnostics_test|physical_round_normalizer_test|paimage_host_output_test|paimage_production_gap_test"`
   → 7/7 PASS（含 DataProcessor legacy B1-B4 所在的 `data_processor_batch_test`）。
6. Windows 完整构建（在 reset-recovery code commit `35953ee` 上）：
   `cmd /c build_mingw_debug.cmd` → **exit 0**
   （configure preset `mingw-debug` + build preset `mingw-debug-build` +
   windeployqt；脚本自检四个必需 exe
   `PAimageReceiverDiagnostics.exe / ImagingSvc.exe / ring_svc_selftest.exe /
   ring_udp_replay.exe` 齐备；链接该改动的
   `PAimageReceiverDiagnostics.exe` 已在本次构建重链，三个不依赖
   PaimageAcquisition 的 exe 由 Ninja 正确跳过）。
   本 commit 之后仅追加文档 commit，依 §19 不再为纯文档重建二进制。

测试 seam 与第一次 addendum 相同：loopback 假卡走真实
`NetworkController → Backend → SocketReceiver → SourceCore → observationSink →
CardStats` 链路，`getAllCardStats()` 断言；无 fake gap calculator。

## 6. Dependencies

本 addendum 不修改 CUDA core，未重新生成 CUDA runtime；依赖与
`SESSION_B_REVIEW_ADDENDUM_RECEIPT_20260917.md` §6 记录一致、本轮未变。

## 7. Acceptance 对照（§21）

1. 同-session large reset recovery —— SourceCore 激活块 backJump>=256 重建锚点，PASS（B-ADD-7/8B）
2. 阈值精确 256，与 DataProcessor 冻结语义对齐 —— `kTriggerResetBackJumpThreshold=256`，PASS
3. reset transition `missingTriggerCount += 0` —— B-ADD-7 T10 后 == 0，PASS
4. reset transition 不加 full-trigger `packetsDropped` 当量 —— B-ADD-7 T10 后 == 0，PASS
5. reset 后新锚点立即生效 —— B-ADD-7 T10→T14 记 +3 / +3*expected，PASS
6. backJump 255 不 re-anchor —— B-ADD-8A（T401 对 T400 保持 adjacent，missing=0），PASS
7. backJump 256 re-anchor —— B-ADD-8B（T144→T148 记 +3），PASS
8. uint16 wrap-forward 不误判 reset —— signed forward 优先判断；B-ADD-4 记 +2，PASS
9. small late/backstep 不误判 reset —— B-ADD-5 / B-ADD-8A，PASS
10. measurement-session reset 仍隔离旧锚点 —— B-ADD-6，PASS
11. admission/assembly/sync/Normalizer 行为不变 —— diff 仅限 §4 文件；
    `paimage_network_test`（真实准入/存储/停启）PASS
12. B-ADD-1..8 全 PASS —— 见 §5.2
13. DataProcessor legacy B1-B4 继续 PASS —— `data_processor_batch_test` PASS
14. Session A/B direct regressions PASS —— 回归子集 7/7，见 §5.5
15. 完整 CTest PASS —— 42/42
16. Windows full build PASS —— exit 0，见 §5.6
17. HANDOFF 不再把 `917a719...` 作为 Session C 最终基线 —— Session C inputs
    已改写为 final remote HEAD + 四个必需 commit 链
18. HANDOFF test count 与真实最终 suite 一致 —— 已更新为 ctest 42/42
19. receipt + HANDOFF 已 push —— 见最终执行报告
20. tracked working tree clean —— 推送前核验
21. local HEAD == remote branch HEAD —— 推送后 `git ls-remote` 核验
22. hardware validation 保持 PENDING —— 本任务全部为 loopback/自动化验证

## 8. Validation boundary

```text
SESSION_B_SOFTWARE_IMPLEMENTATION = PASS
SESSION_B_PAIMAGE_PRODUCTION_GAP_ACCOUNTING = PASS
SESSION_B_SAME_SESSION_SEQ_RESET_RECOVERY = PASS
SESSION_B_AUTOMATED_TESTS = PASS (ctest 42/42)
SESSION_B_WINDOWS_BUILD = PASS
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS = NOT PROVEN BY THIS TASK
```

本轮全部为软件侧 loopback/单元级确定性验证；不得解释为真实 FPGA/NIC/LabVIEW
实机跳号/复位行为已验证。
