# ChatGPT 网页端 + Codex Desktop GitHub 双端联动快速上手

## 目标

本仓库采用 GitHub 作为 ChatGPT 与 Codex Desktop 之间的共享状态总线：

- `main`：唯一 canonical branch，保存正式源码与正式文档基线。
- `codex/task-docs`：仅用于 ChatGPT 发布任务文档，不承载实现代码。
- `codex/<task-name>-<timestamp>` 或其他明确命名的任务分支：由 Codex Desktop 从最新 `main` 创建，用于具体实现。

ChatGPT 负责方案设计、任务规格、代码审查与验收；Codex Desktop 负责具体代码修改、本地构建、测试、提交与推送。

## Codex 开始任务前必须先读

Codex Desktop 获取具体任务文档后、开始实现或构建前，必须先读取 `origin/main` 上的两份仓库级长期规范：

```text
REPOSITORY_BASELINE.md
BUILD_STANDARD.md
```

推荐顺序：

```powershell
git fetch origin
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/<任务文件名>
```

其中：

- `REPOSITORY_BASELINE.md` 管理分支、历史和交付治理；
- `BUILD_STANDARD.md` 管理所有后续工作的默认工具链、CMake preset、构建脚本、CUDA/Qt/ZeroMQ 依赖、BuildIdentity、正式交付目录和二进制回执；
- 具体任务文档决定本次任务的目标分支/commit、特殊测试与明确 override。

`BUILD_STANDARD.md` 存放在 `main` 是为了让它成为稳定入口，**不表示 Codex 应默认在 `main` 上构建**；实际构建对象始终是当前任务指定的实现分支和精确 commit。

## 已验证的连接方式

目标仓库：`ZiXuannnZhang/PAErealtimeImaging`

默认分支：`main`

Codex Desktop 已验证可以通过仓库级 SSH Deploy Key，经 `ssh.github.com:443` 进行无人值守 fetch/push。

推荐工作区：

```text
D:\ChatGPT\PAERealtimeImaging
```

仓库专用 SSH 文件位于：

```text
C:\Users\yyps\.codex\ssh\PAErealtimeImaging
```

其中私钥禁止显示、复制、提交或发送。

## 分支治理

### main

`main` 是唯一正式基线。

任何实现任务必须先同步 `origin/main`，然后从本地 `main` 创建独立任务分支。不得从历史诊断分支、迁移分支、快照分支或 `codex/task-docs` 创建实现分支。

### codex/task-docs

该分支只用于任务文档通信。

任务文档统一放在：

```text
TASKS/
```

任务文件命名规则：

```text
<简要任务说明>_YYYYMMDD-HHMMSS.md
```

示例：

```text
TASKS/RingBlockAssembler安全加固_20260907-153012.md
```

时间戳使用任务发布时的本地时间，精确到秒，用于唯一标识和排序。

`TASKS/README.md` 是长期规范文件，不受上述单次任务命名规则约束。

## 标准任务流程

### 1. ChatGPT 发布任务

ChatGPT 在 `codex/task-docs` 的 `TASKS/` 目录新增任务文档。任务文档至少应包含：

1. Objective
2. Baseline commit / baseline branch
3. Problem and evidence
4. Invariants
5. Prohibited scope
6. Recommended design
7. Implementation freedom
8. Expected changed files
9. Required tests
10. Regression baseline
11. Build/test commands
12. Acceptance criteria
13. Required execution report

任务文档必须明确以 `main` 为实现基线，而不是以任务文档分支为实现基线。

### 2. Codex Desktop 获取任务

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch origin

git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/<任务文件名>
```

也可以临时查看任务文档分支，但不要在该分支上写实现代码。

### 3. Codex Desktop 同步正式基线

```powershell
git switch main
git merge --ff-only origin/main
git status
```

若 `--ff-only` 失败，立即停止，不要 reset、rebase 或 force；先报告本地与远端分叉情况。

### 4. Codex Desktop 创建实现分支

从最新 `main` 创建：

```powershell
git switch -c codex/<task-name>-<timestamp>
```

任务分支名应简短、可识别，并避免与 `codex/task-docs` 混淆。

### 5. 实施与验证

提交前必须检查：

```powershell
git status
git diff
git add -- <明确的文件路径>
git status
git diff --cached
```

只允许提交任务范围内的修改。

执行任务文档要求的构建、测试和回归验证，并记录实际命令和结果。

构建和交付必须同时遵守 `BUILD_STANDARD.md`；如任务文档对工具链、preset、依赖或交付方式有明确 override，必须在执行报告中指出。

### 6. 提交与推送

```powershell
git commit -m "<commit message>"
git push -u origin <task-branch>
```

推送后核验：

```powershell
$LocalSha = git rev-parse HEAD
$RemoteSha = (git ls-remote origin "refs/heads/<task-branch>").Split()[0]
$LocalSha
$RemoteSha
$LocalSha -eq $RemoteSha
```

必须确认结果为 `True`。

### 7. Codex Desktop 返回执行报告

至少报告：

- 任务文档文件名
- 实现分支名
- commit SHA
- 改动文件列表
- 实际执行的构建命令
- 实际执行的测试命令
- 测试结果
- 回归结果
- 正式 build-delivery 路径（如本任务产生可运行产物）
- 构建 preset / 工具链 / 关键依赖来源
- 已知限制或未验证项

不能只报告“测试通过”或“build passed”。

### 8. ChatGPT 审查

ChatGPT 从 GitHub 重新读取实现分支或 PR 的最新 HEAD 和 diff，并对照任务文档逐项检查。

审查结论使用：

- `APPROVE`
- `REQUEST_CHANGES`

默认情况下，最终 merge 由用户决定；除非用户明确授权，ChatGPT 不自动合并实现分支到 `main`。

## Codex Desktop 仓库接入

确认当前环境与 remote：

```powershell
whoami
git --version
ssh -V
git remote -v
git status
```

目标 remote：

```text
git@github.com:ZiXuannnZhang/PAErealtimeImaging.git
```

若当前 origin 不是该 SSH URL：

```powershell
git remote set-url origin git@github.com:ZiXuannnZhang/PAErealtimeImaging.git
```

配置 repository-local SSH：

```powershell
$SshDir = "C:\Users\yyps\.codex\ssh\PAErealtimeImaging"
$KeyPosix = (Join-Path $SshDir "deploy_ed25519").Replace("\","/")
$KnownHostsPosix = (Join-Path $SshDir "github_known_hosts").Replace("\","/")
$SshCommand = "ssh -i $KeyPosix -o IdentitiesOnly=yes -o UserKnownHostsFile=$KnownHostsPosix -o StrictHostKeyChecking=yes -o HostName=ssh.github.com -p 443 -o KexAlgorithms=curve25519-sha256"
git config --local core.sshCommand $SshCommand
git fetch origin
```

检查 repository-local commit identity：

```powershell
git config --local user.name
git config --local user.email
```

若任一项为空，只在当前仓库设置：

```powershell
git config --local user.name "Codex Desktop"
git config --local user.email "codex-desktop@local.invalid"
```

不要修改全局 Git identity。

## 禁止事项

1. 不要直接在 `main` 上实施任务代码。
2. 不要从 `codex/task-docs` 创建实现分支。
3. 不要把实现代码提交到 `codex/task-docs`。
4. 不要从历史快照、诊断或迁移分支作为新任务基线。
5. 不要执行 `git push --force` 或 `git push -f`。
6. 不要在同步失败时自动 `reset --hard`、自动 rebase 或改写历史。
7. 不要提交 SSH 私钥、凭据、PAT 或敏感配置。
8. 不要依赖“测试通过”这一结论；必须记录具体命令、输出结论和未覆盖项。
9. 不要绕过 `BUILD_STANDARD.md` 静默使用另一套编译器、preset、CUDA DLL 或交付目录。

## 当前正式状态

- GitHub 双端链路：已验证。
- `main`：唯一 canonical branch。
- `codex/task-docs`：任务文档专用分支。
- `BUILD_STANDARD.md`：所有后续工作的正式构建与交付规范。
- 新实现任务：必须从最新 `main` 创建独立分支。
- ChatGPT：设计与审查。
- Codex Desktop：实现、本地验证、规范化构建与交付。
