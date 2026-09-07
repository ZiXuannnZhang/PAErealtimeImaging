# Codex Task Channel

本目录只存在于 `codex/task-docs` 任务文档分支，用于 ChatGPT 向 Codex Desktop 发布实施任务。

## 分支职责

- `main`：唯一 canonical branch，保存正式源码和正式项目基线。
- `codex/task-docs`：任务文档通信分支，只存放任务规格和本目录治理文件。
- 实现分支：Codex Desktop 从最新 `main` 创建，用于代码修改、构建与测试。

禁止从 `codex/task-docs` 创建实现分支，也禁止把源码实现提交到该分支。

## 单次任务文件命名

```text
<简要任务说明>_YYYYMMDD-HHMMSS.md
```

要求：

- 简要任务说明应足以区分任务主题，尽量使用稳定的模块名或问题名。
- 时间戳使用任务发布时本地时间，格式为 24 小时制，精确到秒。
- 同一任务的补充说明原则上更新原任务文档；若任务范围发生实质变化，则创建新的时间戳文档。

示例：

```text
RingBlockAssembler安全加固_20260907-153012.md
网络板卡身份识别加固_20260907-161405.md
```

## 任务文档最低结构

每个任务至少包含以下部分：

1. `Objective`
2. `Baseline`
3. `Problem / Evidence`
4. `Invariants`
5. `Prohibited Scope`
6. `Recommended Design`
7. `Implementation Freedom`
8. `Expected Changed Files`
9. `Required Tests`
10. `Regression Baseline`
11. `Build / Test Commands`
12. `Acceptance Criteria`
13. `Required Execution Report`

## Baseline 规则

任务文档中的实现基线必须指向发布任务时最新的 `main` commit SHA。

Codex Desktop 开始任务前必须：

```powershell
git fetch origin
git switch main
git merge --ff-only origin/main
```

然后从该 `main` 创建新的实现分支。

如果本地 `main` 无法 fast-forward 到 `origin/main`，停止任务并报告分叉，不得自动 reset、rebase 或 force。

## Codex Desktop 执行报告

完成任务后至少返回：

- 任务文档文件名
- 实现分支名
- 最终 commit SHA
- 改动文件列表
- 实际构建命令及结果
- 实际测试命令及结果
- 回归验证结果
- 未验证项
- 已知限制

不能用“测试已通过”替代具体证据。

## ChatGPT 审查

ChatGPT 将重新从 GitHub 获取实现分支或 PR 的远端 HEAD、diff 和测试证据，并对照任务文档给出：

- `APPROVE`
- `REQUEST_CHANGES`

默认不自动合并到 `main`，除非用户明确授权。
