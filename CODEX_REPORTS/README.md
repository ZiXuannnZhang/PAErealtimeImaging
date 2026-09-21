# CODEX_REPORTS — execution / validation / historical evidence

本目录保存**非 canonical 的工程执行、验证、诊断和历史设计材料**。当前项目状态与规范不在这里维护。

## 当前主题目录

| 目录 | 内容 |
| --- | --- |
| `ring-reconstruction-history-202608/` | 环扫算法、CUDA、历史 benchmark、M2/M3 里程碑 |
| `acquisition-startup-history-20260907-13/` | PAimage 迁移、启动段诊断、START admission、system capture、ImagingBypass 隔离 |
| `round-identity-history-20260907-16/` | RingBlockAssembler/SHM、PhysicalRound、RoundIdentity 修复与验证 |
| `session-abcd-closeout-20260918/` | Session A–D 最终执行 receipts / handoff / checklist |
| `hardware-env-20260914/` | Realtek USB 10GbE 网卡高级属性快照(只读) |

## 适合放入本目录

- Codex/ChatGPT 执行报告、review/addendum receipt；
- 构建/测试证据、候选交付回执；
- 诊断报告、dump/log/抓取/截图；
- 已完成或被后续 source 吸收的阶段设计；
- 历史 benchmark / 风险评估 / 操作卡。

## 不应放入本目录

canonical 当前入口：

- 根 `README.md`
- `PROJECT_STATUS.md`
- `REPOSITORY_BASELINE.md`
- `BUILD_STANDARD.md`
- `HANDOFF.md`
- `Codex-GitHub双端联动快速上手.md`
- 当前仍直接指导某模块运行的 module README/docs。

## 权威性

历史材料中的 PENDING、旧路径、旧 executable、旧 branch 或旧硬件假设保留其历史语境，
但不覆盖 current source/canonical docs。

同一阶段高度重复的临时说明会合并成 archive README；原始证据有审计价值时保留。
完全被后续总结覆盖且没有独有证据价值的临时 handoff/plan 可以从当前树删除，Git history 仍可恢复。
