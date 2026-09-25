# Codex ↔ GitHub 双端联动快速上手

## 1. Source of truth

`ZiXuannnZhang/PAErealtimeImaging` 的 canonical source 是 `origin/main`。

每次开始工作：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
```

有任务文档时再读：

```powershell
git show origin/codex/task-docs:TASKS/<任务文件名>
```

权威顺序：

1. `PROJECT_STATUS.md`
2. `REPOSITORY_BASELINE.md`
3. `BUILD_STANDARD.md`
4. task-specific document
5. module README/docs
6. historical `CODEX_REPORTS`

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

除非任务明确 override，标准流程：

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

完成任务后：

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
