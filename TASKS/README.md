# Codex Task Channel

本目录只存在于 `codex/task-docs` 任务文档分支，用于 ChatGPT 向 Codex Desktop 发布实施任务。

## 开始任何任务前

Codex 必须先获取远端并读取当前仓库级状态/规范：

```powershell
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/<任务文件名>
```

其中：

- `PROJECT_STATUS.md` 决定当前哪些分支/commit 正在开发、待验证或暂缓合并；
- `REPOSITORY_BASELINE.md` 决定 branch/history/merge 治理；
- `BUILD_STANDARD.md` 决定默认构建、依赖、产物和交付方式；
- 当前 task document 决定本任务的精确 baseline、目标分支、测试和显式 override。

根 `HANDOFF.md` 已降级为历史兼容入口，不得作为当前任务状态依据。

## 分支职责

- `main`：唯一 canonical branch，保存正式源码和仓库级正式文档。
- `codex/task-docs`：任务文档通信分支，只存放任务规格和本目录治理文件。
- 实现分支：用于具体代码修改、构建与测试。

禁止从 `codex/task-docs` 创建实现分支，也禁止把源码实现提交到该分支。

## Baseline 规则

### 新任务

普通新实现任务的 baseline 必须指向任务发布时最新的 `main` commit SHA。

Codex 开始时：

```powershell
git switch main
git merge --ff-only origin/main
git switch -c codex/<task-name>-<timestamp>
```

### 已存在实现分支的追加/验收任务

如果任务文档或 `PROJECT_STATUS.md` 明确指定继续某个已经存在的实现分支，则**不得**新建分支，也不得先把最新 `main` merge/rebase 到该实现分支。

应执行：

```powershell
git switch <existing-task-branch>
git merge --ff-only origin/<existing-task-branch>
git rev-parse HEAD
```

并核对 task 指定的精确 starting SHA。

当前 START admission 实机候选即属于这种情况：

```text
codex/start-admission-fence-fix-20260913-003112
6313540f72544c0f68820c4815903abaa0b8c1e1
```

其软件验收已 `APPROVE`，但真实 FPGA/NIC 实机验证尚未完成，当前保持未合并 `main`。除非另有明确任务，不得为了同步 `main` 而改变该候选源码。

## 单次任务文件命名

```text
<简要任务说明>_YYYYMMDD-HHMMSS.md
```

时间戳使用任务发布时本地时间，24 小时制，精确到秒。

同一任务的小范围 review addendum 可以继续原实现分支；任务范围发生实质变化时应创建新的任务文件，并明确 branch/baseline 规则。

## 任务文档最低结构

每个任务至少包含：

1. `Objective`
2. `Baseline / exact starting SHA`
3. `Target implementation branch`
4. `Problem / Evidence`
5. `Invariants`
6. `Prohibited Scope`
7. `Recommended Design`
8. `Implementation Freedom`
9. `Expected Changed Files`
10. `Required Tests`
11. `Regression Baseline`
12. `Build / Test Commands or BUILD_STANDARD reference`
13. `Acceptance Criteria`
14. `Required Execution Report`
15. `Hardware / system validation boundary`（如适用）

## 构建与交付

除非任务明确 override，统一遵守最新 `origin/main:BUILD_STANDARD.md`。

注意：构建规范位于 `main` 不等于构建对象必须是 `main`。如果任务指定现有实现分支/commit，应在该目标 commit 上构建，同时使用最新构建规范。

正式交付应报告 exact SHA、preset/toolchain、关键依赖来源/hash、native bin、build-delivery staging 以及 task-required tests。

## Codex Desktop 执行报告

完成任务后至少返回：

- 任务文档文件名；
- 实现分支名；
- starting SHA / final SHA；
- 改动文件列表；
- 实际构建命令、preset、工具链及结果；
- 实际测试命令及结果；
- 正式交付目录（如有）；
- 关键 runtime/dependency 来源与 hash（如有）；
- 回归验证结果；
- 未验证项；
- 已知限制；
- local HEAD == remote branch HEAD 的回执。

不能用“测试已通过”替代具体证据。

## ChatGPT 审查

ChatGPT 会重新从 GitHub 获取：

```text
remote HEAD
baseline / merge-base
base -> head diff
changed-file scope
source
required tests/reports
```

并独立给出：

- `APPROVE`
- `REQUEST_CHANGES`

Codex 自报通过不是验收证据本身。

## Merge 与硬件验证

默认 merge 由用户决定。

软件 `APPROVE` 不自动意味着硬件/系统验收完成。若真实设备测试仍待完成，实现分支可以长期保持未合并状态，并作为明确的 hardware-test candidate。

不得为了“让仓库看起来整洁”提前合并、squash、rebase 或改写这种候选分支。

## 禁止事项

- 不从 `codex/task-docs` 创建实现分支；
- 不把实现源码提交到本分支；
- 不在同步失败时自动 `reset --hard` / rebase / force；
- 不从历史/backup/实验分支隐式开始新任务；
- 不忽略 `PROJECT_STATUS.md` 中的当前保留/待验证分支；
- 不把历史 `HANDOFF.md` 当当前状态入口；
- 不把软件测试通过表述成真实硬件问题已经解决。
