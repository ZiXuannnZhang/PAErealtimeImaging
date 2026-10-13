# 反证记录（验收第 3 条）

操作：临时把「达成即收口」测试块的 `options.expectedCardCount = 4;` 改为 `0;`
（tests/paimage_core/discovery_checks.cpp 中 6bca496 移植版第 1 个新增块，
原行 350），重建 paimage_discovery_checks 后运行；随后完整还原、重建复跑。

`require()` 失败即抛 `std::runtime_error` 中止，一次运行只暴露首个红断言；
为对任务文档点名 3 条断言逐条取证，依次临时中性化（`true ||` 前缀 +
`COUNTER-PROOF NEUTER` 注释）点名断言之前的断言，分多次运行：

| 运行 | 临时改动 | 结果文件 | 结果 |
| --- | --- | --- | --- |
| run1 | 仅 4→0 | 22 | RED：`expected four cards close discovery`（块首断言，exit 3） |
| pass A | 4→0 + 中性化前 2 条 | 23 | RED：`closed in round 1`（exit 3） |
| pass B | 4→0 + 中性化前 3 条 | 24 | RED：`one CONFIG per candidate, no re-sends`（exit 3） |
| pass C 首跑 | 4→0 + 中性化前 5 条 | 25 | 仍报 `one CONFIG per candidate, no re-sends`（见下） |
| pass C 重跑 | 删除残留行后 | 26 | RED：`late fifth card not admitted`（exit 3） |
| 还原后 | 还原 6bca496 原文（vs 6bca496 diff=0 行） | 27 | GREEN：`PASS discovery policy...`（exit 0） |

## 操作瑕疵记录（如实）

pass B 的临时编辑误在 new_string 中多带了一行 sends 断言，造成块内出现一行
重复的活跃 sends 断言；pass C 首跑因此仍报 sends 断言（25 号文件佐证）。
删除该残留行后 pass C 重跑成功。该瑕疵只存在于反证的临时工作副本中，
未影响：移植内容本身、既有断言、最终还原后的测试文件。

## 还原验证

- 还原后该文件相对参考 6bca496 的 diff = 0 行（逐字节一致，复核于 2026-09-28 21:58 前后）；
- 28 号文件 = 还原后相对移植前基线 1b7c4c2 的完整 diff（恰为 2 个 A hunk）；
- 既有断言逐字未改（require 43 / requireState 6 全保留），新增 15 条
  （require 13 + requireState 2）全部通过（20/27 号文件）。

## 结论

`expectedCardCount` 4→0 使任务文档点名 3 条断言
（`closed in round 1` / `one CONFIG per candidate, no re-sends` /
`late fifth card not admitted`）全部转红，还原后复绿——
「达成即收口」语义真实生效，测试对被测行为具备判别力。
