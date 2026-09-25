# 执行代理 ↔ 分析代理 双端联动快速上手（原 Codex ↔ GitHub 双端联动）

## 0. 双端角色定义

本文以功能角色称呼双端 Agent，不绑定任何具体产品、模型或厂商：

- **执行代理**（原称 Codex 端 / 本地端）：在本地工作区 checkout 源码、实施改动、运行构建与测试、推送实现分支的一端。
- **分析代理**（原称网页端 / ChatGPT 端 / GitHub 端）：不使用本地工作区，直接以 GitHub 远端（`origin/main`、远端分支与提交历史）为对象进行状态分析、文档治理、独立审查与规划的一端。

两条共通规则：

- 角色是功能位。任何 Agent 接手时按本节对号入座，不得因自己的产品名与历史称谓不符而推断本文件不适用于自己。
- 仓库中的分支前缀 `codex/`（含 `codex/task-docs`）与目录 `CODEX_REPORTS/` 是既有命名，保持不变；它们不再暗示只有某一产品可以充当执行代理或分析代理。

## 1. Source of truth

`ZiXuannnZhang/PAErealtimeImaging` 的 canonical source 是 `origin/main`。

执行代理每次开始工作：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/main:HANDOFF.md
```

分析代理没有本地工作区，对同一组对象（上述四份文档、相关分支与提交历史）直接通过 GitHub 远端 API 读取核对，并记录 main exact SHA；不执行本地 fetch。

有任务文档时再读：

```powershell
git show origin/codex/task-docs:TASKS/<任务文件名>
```

权威顺序（与 `README.md`「Canonical documentation」是同一套清单）：

1. `PROJECT_STATUS.md`
2. `REPOSITORY_BASELINE.md`
3. `BUILD_STANDARD.md`
4. `HANDOFF.md`（接力入口，不替代上述三份治理文档）
5. `Codex-GitHub双端联动快速上手.md`（协作方式，本文件）
6. `MC_410T_MultiCard/delivery/README.md`（当前生产采集/实时成像架构）
7. `CODEX_REPORTS/README.md`（历史执行、验证与诊断档案说明）

任务特殊要求以 `codex/task-docs:TASKS/<task>.md` 为准，可覆盖上述治理文档中与之冲突的条款。

## 2. Current project state

截至 2026-09-25，START-admission 与 Session A/B/C/D accepted source chain 已集成进入 main；
2026-09-24 另有已过实机验收的前端链（全分辨率显示裁切、Frontend Preprocessing Stage、
逐 A-line 零相位前端滤波、保存/闸门与写盘故障修复、显示命名）以 fast-forward 进入 main。
main 的源码树身份为 `491aa34cfa9553954eea949a7703df7194eb7699`，其后 main 仅因文档提交移动 HEAD。
当前实机状态：

```text
A/B/C/D functional validation = PASS_FOR_CURRENT_SCOPE
exact FPGA/LabVIEW source of extra startup triggers = NOT_PROVEN
```

因此不要再把 START/Session C/Session D/integration branch 当作“尚未合并的 current candidate”。
它们现在是 traceability branches。

## 3. New implementation task

除非任务明确 override，执行代理的标准流程：

```powershell
git fetch --prune origin
git switch main
git merge --ff-only origin/main
git status --short
git switch -c codex/<task-name>-<timestamp>
```

若 local main 不能 ff-only：

- 停止；
- 报告 local/main 与 origin/main SHA；
- 盘点 local-only commits；
- 不 reset/rebase/force。

## 4. Existing reviewed task branch

只有 task document 明确要求继续某个历史实现分支时，才进入该 branch，并使用：

```powershell
git fetch --prune origin
git switch <branch>
git merge --ff-only origin/<branch>
```

不要为了“同步最新文档”把 main merge 到任务 branch，除非任务明确要求。

## 5. Task-doc channel

`codex/task-docs` 只保存任务规格，不是 production baseline。

读取：

```powershell
git show origin/codex/task-docs:TASKS/<task>.md
```

实施代码不要提交到 `codex/task-docs`。

## 6. Submission

执行代理完成任务后：

1. 确认 tracked tree；
2. 记录 exact source SHA；
3. 按任务要求跑 tests/build/selftest；
4. commit；
5. push implementation branch；
6. 验证 local HEAD == remote HEAD；
7. 返回 branch / SHA / changed files / test evidence。

禁止：

- force push；
- 在 main 上直接做未审查功能开发；
- 把软件 PASS 写成硬件 PASS；
- 用旧 branch 的 build evidence 证明新 SHA；
- 隐式跳过 BUILD_STANDARD。

## 7. Build / delivery

正式 Windows build 统一：

```powershell
Set-Location "<repo>\MC_410T_MultiCard\delivery"
cmd /c build_mingw_debug.cmd
```

必须在待交付 exact commit 上重新 configure。runtime dependency provenance、SHA256、staging
和 BuildIdentity 以 `BUILD_STANDARD.md` 为准。

## 8. Hardware evidence language

始终区分：

```text
source/code correctness
automated tests/build/selftest
real hardware validation
hardware root-cause attribution
```

当前项目已经接受的是“当前实机验证范围通过”；额外 trigger 的底层精确来源仍未证明。
如果以后出现新的硬件行为，只记录可观察事实，不把 7/4007 硬编码成协议真值。

## 9. Historical material

`CODEX_REPORTS/` 保存历史执行/验证/诊断证据。旧报告可解释设计演进，但不覆盖 current main。

不要恢复已经删除的 `codex/local-docs-sync-20260913`。
