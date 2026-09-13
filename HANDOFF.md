# HANDOFF — 历史兼容入口（已降级）

> **本文件不再是“新会话唯一入口”。**
>
> 2026-09-05/06 的 `realtime_imaging_migration` 交接内容已经过时，其中包含旧工作区、旧 remote、旧 `build/mingw_make` 构建目录、本地未提交改动和迁移阶段沙箱说明。它们不得用于当前分支选择、构建、交付或本地工作区整理。

## 当前必须读取的项目级入口

按以下顺序读取：

1. `PROJECT_STATUS.md` — 当前项目状态、活跃/待验证分支、下一步工作。
2. `REPOSITORY_BASELINE.md` — canonical branch、分支角色、历史和 merge 治理。
3. `BUILD_STANDARD.md` — 默认工具链、CMake preset、CUDA/Qt/ZeroMQ、构建与交付规范。
4. `Codex-GitHub双端联动快速上手.md` — ChatGPT / Codex Desktop 协作流程。
5. 当前 `codex/task-docs:TASKS/<task>.md` — 本次任务的精确目标、commit、测试和显式 override。

## 当前关键状态（2026-09-13）

- `main`：唯一 canonical branch。
- `codex/task-docs`：仅任务规格，不是实现基线。
- `codex/start-admission-fence-fix-20260913-003112@6313540f72544c0f68820c4815903abaa0b8c1e1`：START admission 软件修复已完成独立软件验收并获 **APPROVE**，但真实 FPGA/NIC 实机测试尚未完成，因此**保持未合并 `main`**。
- `codex/local-docs-sync-20260913`：用户已手动删除，不是正式成果，不应恢复或作为本地整理依据。
- `codex/ring-pipeline-refactor-20260912`：独立未合并方向，不得与当前 START 候选或 `main` 隐式混用。

当前状态以 `PROJECT_STATUS.md` 为准；这里的摘要仅用于兼容仍然首先打开 `HANDOFF.md` 的旧工作流。

## 历史迁移记录

原 `HANDOFF.md` 的迁移期内容已降级为历史资料。历史索引见：

```text
docs/history/HANDOFF_realtime_imaging_migration_20260905-06.md
```

完整旧文本仍可从 Git 历史恢复，例如：

```text
main@1a49c2f1eb2d9769be6829cdd6e36dc507f93ad5:HANDOFF.md
```

历史记录可用于技术考古，但其中旧绝对路径、旧构建命令、旧 remote、本地状态和“待提交改动”不得覆盖当前仓库级规范。
