# CODEX_REPORTS — 执行、验证与历史工程证据归档

`CODEX_REPORTS/` 用于保存**非 canonical 的阶段性工程材料**。这里的内容可用于追溯设计决策、执行过程、测试证据和问题诊断，但不得覆盖仓库根目录的当前项目状态与治理文档。

## 适合放入本目录的内容

- Codex / ChatGPT 任务执行报告、审查回执、addendum receipt；
- 阶段性 handoff、候选交付回执、测试/构建证据；
- 诊断报告、崩溃调查、抓取证据、截图；
- 已完成或已被后续实现吸收的阶段性设计/实施方案；
- 历史性能基准、风险评估、操作卡；
- 与上述报告直接配套的 raw CSV / JSON / log / evidence 子目录。

## 不应放入本目录的内容

以下属于 canonical 项目入口，应保留在仓库根目录或稳定的产品文档目录：

- `README.md`
- `PROJECT_STATUS.md`
- `REPOSITORY_BASELINE.md`
- `BUILD_STANDARD.md`
- `HANDOFF.md`
- `Codex-GitHub双端联动快速上手.md`
- 当前仍作为正式规范维护的架构/API/用户文档。

## 权威性规则

1. 当前状态以根目录 `PROJECT_STATUS.md` 为准。
2. 分支与历史治理以 `REPOSITORY_BASELINE.md` 为准。
3. 构建/交付以 `BUILD_STANDARD.md` 为准。
4. `CODEX_REPORTS/` 中任何历史结论若与当前 source / canonical docs 冲突，均以当前 source / canonical docs 为准。
5. 历史报告可以保留当时的 PENDING/假设/路径信息，但应通过所在目录 README 或文件前言标明其历史身份。

## 目录整理原则

- 同一阶段多份高度重叠的临时说明，应优先合并为一个索引/总结，精确执行证据放入子目录；
- 原始证据仍有审计价值时保留；
- 已被后续总结完全覆盖、没有独有证据价值的临时 handoff/notes 可以从当前树删除，Git 历史仍可恢复；
- 不为“整洁”修改历史报告里的原始测试数字或结论，只通过归档说明界定其适用范围。
