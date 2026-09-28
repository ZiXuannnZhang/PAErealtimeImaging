# discovery-earlyexit-port-20260928-214240

卡发现早退移植至环形增强分支（移植任务，A 范围仅）。

- 任务文档：`codex/task-docs @ 4eb0e8d` `TASKS/卡发现早退移植至环形增强分支_20260928-213124.md`
- port commit：`6744787fc954acb449d26795b6752ff8139aec2d`（transplanted from `6bca496`，A/B 范围切分见提交信息）
- 参考实现：`codex/discovery-early-exit-localbind-20260926-123437 @ 6bca496`
- 实机验收项留给用户；软件 / 单测 PASS ≠ 实机 PASS

## 文件索引

| 文件 | 内容 |
| --- | --- |
| 00_preport_state.txt | 移植前基线状态（HEAD=1b7c4c2、tracked clean、task-docs tip 核对） |
| reference_6bca496_full.diff | 参考提交完整 diff（16 hunk = A 12 + B 4，逐字对照基准） |
| 01_ctest_baseline_prerun.txt | 移植前基线全量 CTest（首跑 50/51 + bench 间歇 SegFault） |
| 02_ctest_target_list.txt | 基线 CTest 目标清单（51） |
| 03_ctest_port_full.txt | 移植后（pre-commit 工作树）全量 CTest 51/51 首跑全绿 |
| 04_ctest_port_target_names.txt | 移植后目标名清单（51，与 05 逐行 diff 相等） |
| 05_ctest_baseline_target_names.txt | 基线目标名清单（51） |
| 06_ctest_port_final_at_6744787.txt | 提交态 6744787 上复跑全量 CTest 51/51 全绿（最终口径） |
| 10_check_A_files_vs_reference.txt | §1.2 对照一：A 范围 3 文件 vs 6bca496 diff=0 行 |
| 11_check_Constants_vs_preport.txt | §1.2 对照二：Constants.h vs 移植前 diff=0 行（B 未引入） |
| 12_check_MainWindow_vs_preport.diff | §1.2 对照三：MainWindow.cpp vs 移植前 = 恰 3 处 A hunk，含逐字比对结论 HUNK-BODY-IDENTICAL |
| 20_discovery_checks_green_prerun.txt | 反证前基线运行：全绿（exit 0） |
| 21_counterproof_temp_edit.diff | 反证临时改动留痕（4→0 + 中性化；已还原） |
| 22~27 号文件 | 反证系列：4→0 转红（run1/passA/passB/passC）+ 还原复绿 |
| 28_discovery_checks_diff_vs_preport.diff | 移植后测试文件 vs 移植前完整 diff（= 参考 2 hunk） |
| 29_counterproof_README.md | 反证方法、点名 3 断言逐条转红记录、操作瑕疵如实记录 |
| 30_mw_A_only_patch.diff / 31_mw_B_only_patch.diff | 从参考 diff 机械切分的 MainWindow A/B hunk 补丁 |
| 40~43 号文件 | Debug/Release 构建日志（42/43 为 Release 前两次失败现场；.txt 存档，仓库 .gitignore 忽略 *.log，沿分支先例） |
| 44_BuildIdentity_debug.h / 45_BuildIdentity_release.h | 两个构建口径的 BuildIdentity |
| 46_dependency_sha256.txt | 关键运行时依赖 SHA256 |
| git-receipt.txt | 提交链、操作记录、红线自查 |
| BuildIdentity.txt | BuildIdentity 汇总与字段口径说明 |

## 验收对照（任务文档「验收」节）

1. 既有断言不变：require 43 / requireState 6 逐字保留，全绿 ✓
2. 新增断言：require 13 + requireState 2 = 15 条，全绿 ✓
3. 判别性反证：4→0 后点名 3 断言（closed in round 1 / one CONFIG per candidate,
   no re-sends / late fifth card not admitted）逐一转红，还原复绿 ✓（29 号）
4. 全量回归：提交态 51/51，目标集与基线 51 目标逐行一致；基线首跑 ring_enhancer_bench
   间歇 SegFault 1 次、复跑 3/3 绿（任务文档预警的 physical_round_normalizer_test 本轮
   基线/移植后均直接通过，如实记录）✓
5. 构建：MinGW-Debug（正式）+ MinGW-Release（参考）均 exit 0，BuildIdentity 双口径 ✓
6. 证据：本目录 ✓
7. 提交：port commit 6744787（信息含 transplanted from 6bca496 与 A/B 切分），push 后
   local HEAD == remote HEAD（见分支 git log）✓
8. docs/环形重建增强.md 末尾「6. 卡发现早退（移植）」小节随 port commit ✓
9. 实机验收：**未执行，留给用户**（NCards=现场卡数提前收口日志、不足时回落全预算、
   迟到多余卡不误纳；由用户按既有口径记录）
