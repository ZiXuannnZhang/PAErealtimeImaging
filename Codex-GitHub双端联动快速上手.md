# ChatGPT 网页端 + Codex Desktop GitHub 双端联动快速上手

## 目标

GitHub 是 ChatGPT 与 Codex Desktop 之间的唯一共享状态总线。

- `main`：唯一 canonical branch，保存正式源码与仓库级正式文档。
- `codex/task-docs`：任务规格专用分支，不承载实现代码。
- `codex/<task-name>-<timestamp>`：具体实现/验证分支。

ChatGPT 负责架构、技术判断、任务规格、远端独立审查与验收；Codex Desktop 负责本地修改、Windows 构建、测试、产物交付和推送执行报告。

## Codex 开始任何工作前必须先读

无论是开发、构建、实机测试准备还是本地工作区整理，先执行：

```powershell
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
```

如果存在具体任务，再读取：

```powershell
git show origin/codex/task-docs:TASKS/<任务文件名>
```

优先级：

1. `PROJECT_STATUS.md`：当前项目状态、待验证分支/commit、下一步。
2. `REPOSITORY_BASELINE.md`：分支和历史治理。
3. `BUILD_STANDARD.md`：构建、依赖、交付治理。
4. 当前 task document：任务特定目标、测试和显式 override。

根 `HANDOFF.md` 已降级为历史兼容入口，不得覆盖上述当前文档。

## 当前项目特别状态

截至 2026-09-13，START admission 软件修复已完成独立软件验收并获 `APPROVE`，但真实 FPGA/NIC 实机测试未完成，因此保持在：

```text
codex/start-admission-fence-fix-20260913-003112
6313540f72544c0f68820c4815903abaa0b8c1e1
```

暂不合并 `main`。

若任务是生成这次实机测试产物，实际构建对象是上面的分支/SHA，但构建规则来自最新 `origin/main:BUILD_STANDARD.md`。不要为了使用最新文档而把 `main` 源码 merge 到候选分支。

用户已删除 `codex/local-docs-sync-20260913`；`git fetch --prune origin` 后不要恢复该分支。

## 推荐工作区与远端

推荐工作区：

```text
D:\ChatGPT\PAERealtimeImaging
```

目标 remote：

```text
git@github.com:ZiXuannnZhang/PAErealtimeImaging.git
```

仓库专用 SSH 文件：

```text
C:\Users\yyps\.codex\ssh\PAErealtimeImaging
```

私钥禁止显示、复制、提交或发送。

## 标准任务流程

### 1. 获取远端状态

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch --prune origin
git status
git branch -vv
git worktree list
```

### 2. 读取项目状态与任务

```powershell
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/<任务文件名>
```

### 3. 新实现任务同步 main

仅对“从最新 main 开始的新任务”：

```powershell
git switch main
git merge --ff-only origin/main
```

若失败，停止并报告分叉；不得自动 reset/rebase/force。

随后：

```powershell
git switch -c codex/<task-name>-<timestamp>
```

### 4. 已存在实现分支的继续验证/追加任务

若 task 或 `PROJECT_STATUS.md` 明确指定现有实现分支，则不要新建分支，也不要先把 main 合进去。

```powershell
git switch <existing-task-branch>
git merge --ff-only origin/<existing-task-branch>
git rev-parse HEAD
```

必须核对精确 SHA。

### 5. 实施、构建、验证

提交前检查：

```powershell
git status
git diff
git add -- <明确文件>
git diff --cached
```

构建与交付遵守 `BUILD_STANDARD.md`；任务要求的测试必须给出实际命令和结果。

### 6. 提交与推送

```powershell
git commit -m "<commit message>"
git push -u origin <task-branch>
```

核验：

```powershell
$LocalSha = git rev-parse HEAD
$RemoteSha = (git ls-remote origin "refs/heads/<task-branch>").Split()[0]
$LocalSha
$RemoteSha
$LocalSha -eq $RemoteSha
```

必须为 `True`。

### 7. Codex 执行报告

至少报告：

```text
任务文档
实现分支
最终 SHA
改动文件
构建命令和结果
测试命令和结果
正式 build-delivery 路径（若有）
工具链/preset/关键依赖来源
回归结果
未验证项和限制
```

不得只写“build passed”或“tests passed”。

### 8. ChatGPT 独立审查

ChatGPT 必须重新读取远端 HEAD、base→head diff、源码、测试/报告证据，再给：

```text
APPROVE
REQUEST_CHANGES
```

Codex 自报通过不能替代独立验收。

### 9. Merge

默认 merge 仍由用户控制。软件 `APPROVE` 也不自动等于允许合并；如果硬件/系统验收尚未完成，可继续保留实现分支不进入 `main`。

## 本地工作区整理规则

当用户要求“以远端仓库为标准整理本地”时：

1. `git fetch --prune origin`。
2. 先盘点 local branches、worktrees、tracked modifications、untracked/ignored dependencies、unpushed commits。
3. 本地 `main` 只允许 `--ff-only` 到 `origin/main`。
4. 分叉时停止报告，不自动 `reset --hard` / rebase / force。
5. 保留 `PROJECT_STATUS.md` 标记的待验证分支，例如当前 START candidate `6313540f...`。
6. 不因 Git 中不存在就删除本地 CUDA runtime、`artifacts/`、build evidence、抓包或用户数据。
7. 历史 local branch 只有在确认无独有未推送提交后才可清理。
8. 已从远端删除的分支（如 `codex/local-docs-sync-20260913`）在 prune 后可清除对应 stale remote-tracking ref，但不要重建。

## 禁止事项

- 不直接在 `main` 实施任务代码。
- 不从 `codex/task-docs` 创建实现分支。
- 不把源码提交到 `codex/task-docs`。
- 不从历史/backup/实验分支隐式开始新任务。
- 不 force push。
- 不自动 reset/rebase 改写共享历史。
- 不绕过 `BUILD_STANDARD.md` 静默更换编译器、preset、CUDA DLL 或交付目录。
- 不把历史 `HANDOFF.md`、旧 build cache 或旧绝对路径当当前规范。
- 不把“软件测试通过”直接写成“实机问题已解决”。
