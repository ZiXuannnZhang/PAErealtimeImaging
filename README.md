# PAErealtimeImaging

PAErealtimeImaging 是 Windows 平台实时光声成像项目工作区，包含多卡采集、实时数据处理、环形扫描重建、CUDA 重建服务、模拟器、诊断、测试以及半径标定工具。

## Canonical branch

`main` 是本仓库唯一 canonical branch。

所有正式开发任务必须以最新 `origin/main` 为基线创建独立实现分支；任何历史快照、迁移分支、诊断分支均不得作为新的开发基线。

## ChatGPT ↔ Codex Desktop 联动

- 任务文档专用分支：`codex/task-docs`
- 任务目录：`TASKS/`
- 任务文件命名：`<简要任务说明>_YYYYMMDD-HHMMSS.md`
- ChatGPT：负责方案设计、任务规格、代码审查与验收。
- Codex Desktop：负责从 `main` 创建实现分支、修改代码、执行本地构建/测试并推送结果。
- 实现分支不得从 `codex/task-docs` 创建；`codex/task-docs` 仅作为任务文档通道。

详细联动方式见 `Codex-GitHub双端联动快速上手.md`。

## 主要入口

- `HANDOFF.md`：项目整体状态与历史技术决策。
- `MC_410T_MultiCard/delivery/README.md`：实时采集与成像主程序。
- `MC_410T_MultiCard/delivery/docs/`：阶段性设计与验收文档。
- `RadiusCalibration/HANDOFF.md`：多通道扫描半径标定工作。
- `REPOSITORY_BASELINE.md`：仓库基线与分支治理规范。
