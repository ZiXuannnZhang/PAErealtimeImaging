# PAimage START admission 语义修复 — 最终验收收口任务

发布时间：2026-09-13 09:54 +08:00  
任务执行端：Codex Desktop  
执行模型：GPT-5.6 Luna  
思考强度：Max  
任务性质：**最终验收收口；仅补硬断言与最终 Git/报告回执**

---

## 1. Context

原任务：

```text
TASKS/PAimage_START_admission语义修复_20260913-003112.md
```

第一次验收整改：

```text
TASKS/PAimage_START_admission验收整改追加_20260913-021700.md
```

正式代码基线仍为：

```text
origin/main
f326056ee99e5f9c135635d7e97fe2027ee50fbc
```

必须继续使用原实现分支：

```text
codex/start-admission-fence-fix-20260913-003112
```

本任务发布前核实的该分支远端 HEAD：

```text
1e53769729d0d76232c86f886e3d3e87a8af8c74
```

第二轮独立验收结论：

```text
REQUEST_CHANGES
```

但当前 START Fence 生产实现、`completeStart(false)` fail-closed 修复、overflow 逻辑、analyzer 方案 B 均未发现新的生产级阻塞问题。

本任务只处理最后两个验收缺口：

1. injected START send failure 的零 release / 零 CardFrame / 零 SyncFrame 目前只是被记录，没有成为测试硬门禁；
2. execution report 的 Git receipt 仍描述前一个源码提交，而不是最终待审查远端树。

**禁止重新设计或修改已经通过审查的 START Fence 主逻辑。**

---

## 2. Branch / History Rules

开始前执行：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch origin
git switch codex/start-admission-fence-fix-20260913-003112
git merge --ff-only origin/codex/start-admission-fence-fix-20260913-003112
git rev-parse HEAD
git rev-parse origin/codex/start-admission-fence-fix-20260913-003112
git status
```

开始时 local / remote HEAD 必须都是：

```text
1e53769729d0d76232c86f886e3d3e87a8af8c74
```

若远端分支已变化：

```text
STOP
```

并报告，不要 reset、rebase、force、另开分支或自行选择新起点。

本任务：

- 不新建实现分支；
- 不修改 `main`；
- 不 merge validation branch；
- 不 squash/rebase 既有实现历史；
- 继续普通 fast-forward push 到同一实现分支。

最终正式审查仍以：

```text
origin/main@f326056e... -> implementation branch final HEAD
```

为完整范围。

---

## 3. Required Fix A — START send failure 必须成为硬门禁

### 3.1 Current gap

当前 `paimage_start_race_test.cpp` 的 injected failure session 已经记录：

```text
startReturnedFalse
fenceReturnedFalse
heldCount
failedDiscardCount
releasedCount
cardFrameCount
syncFrameCount
recoverySessionPassed
```

实际运行结果已经显示：

```text
startReturnedFalse = true
fenceReturnedFalse = true
heldCount > 0
failedDiscardCount > 0
releasedCount = 0
cardFrameCount = 0
syncFrameCount = 0
recoverySessionPassed = true
```

但当前总测试 `passed` 没有把以下失败 session 指标全部纳入硬断言：

```text
releasedCount == 0
cardFrameCount == 0
syncFrameCount == 0
heldCount > 0
failedDiscardCount > 0
```

这意味着未来若 START 返回失败但错误释放 held 数据，甚至产生 failed-session frame，测试仍可能 PASS。

### 3.2 Required hard assertions

在 `runFailureAndRecovery()` 中，在切换到 recovery session 之前，必须对 failed session 的最终 metrics 做硬断言。

建议最小实现：

```cpp
require(failure_.startReturnedFalse, ...);
require(failure_.fenceReturnedFalse, ...);
require(failure_.heldCount > 0, ...);
require(failure_.failedDiscardCount > 0, ...);
require(failure_.releasedCount == 0, ...);
require(failure_.cardFrameCount == 0, ...);
require(failure_.syncFrameCount == 0, ...);
```

等价方式也可，例如把同样条件加入总 `passed`，但优先使用直接 `require()`，失败时诊断更明确。

### 3.3 Ordering requirement

必须先等待失败 session 的 ingress/drain 已稳定，再读取 metrics 和断言。

不要在还有 in-flight datagram 时过早检查零输出。

当前已有：

```text
waitFor failed session ingress drained
```

可以保留，并在该 wait 完成后进行硬断言。

### 3.4 Recovery remains required

硬断言通过后，仍必须执行现有 recovery session，并要求：

```text
recoverySessionPassed == true
```

确保 fail-closed 不会污染下一次 START。

---

## 4. Explicit Non-Goals for Source Code

除非发现编译问题，本任务原则上**不应修改任何 production source**。

尤其禁止修改：

```text
SocketReceiver.cpp/.h production semantics
Backend.cpp
ControlSocket.cpp/.h
SourceCore.cpp/.h
HostOutput / OutputWorkers
startup_diagnostics_analyze.py behavior
START Fence hold limits
stage1/stage2/stage6 trace schema
```

本次代码改动预计只需：

```text
MC_410T_MultiCard/delivery/tests/paimage_start_race_test.cpp
CODEX_REPORTS/start-admission-fence-fix-20260913/implementation-report.md
```

如果你认为必须修改 production code 或 analyzer 才能完成本任务：

```text
STOP AND ASK
```

说明新发现的问题，不要自行扩大整改。

---

## 5. Required Validation

由于本任务只增加失败场景硬门禁，不要求重新设计，但必须证明最终分支仍满足已经通过的关键回归。

### 5.1 Build relevant tests

使用当前已有 tests build 环境重新构建修改后的 test target。

至少确保：

```text
paimage_start_race_test
```

重新编译成功。

### 5.2 Run race + analyzer pair

必须运行：

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release -R '^paimage_start_race$' --output-on-failure
ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release -R '^paimage_start_race_analyzer$' --output-on-failure
```

或等价的一次正则执行：

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release -R 'paimage_start_race(_analyzer)?' --output-on-failure
```

必须确认 analyzer 读取的是这次新生成的 race artifact，而不是旧 run。

### 5.3 Re-run final focused gates

同时重新运行：

```text
paimage_start_fence_regression
paimage_start_overflow
startup_diagnostics_analyze_test
paimage_network_test
paimage_core full suite
```

生产源码没有变化时，不要求因本任务再次引入构建系统整改。

如果环境允许，记录 production MinGW Debug build 仍可通过；若没有重新全量构建，但 production source 与上一已验证 commit 完全一致，可以在报告中明确：

```text
production source unchanged from previously built/verified commit 2903a2b...
```

并提供 `git diff 1e537697...<final source commit> -- production paths` 为空的证据。

### 5.4 Required failure-session evidence

本次最终 race result 必须明确满足：

```text
failure.startReturnedFalse = true
failure.fenceReturnedFalse = true
failure.heldCount > 0
failure.failedDiscardCount > 0
failure.releasedCount = 0
failure.cardFrameCount = 0
failure.syncFrameCount = 0
failure.recoverySessionPassed = true
```

并且这些条件由 test executable 自身硬断言，不是仅由 report 手工查看。

---

## 6. Required Fix B — Final Git Receipt

更新原报告：

```text
CODEX_REPORTS/start-admission-fence-fix-20260913/implementation-report.md
```

不要新建另一份主报告。

### 6.1 Avoid impossible self-reference

报告文件自身提交后会产生新的 branch HEAD，因此不要声称报告正文可以预先包含“包含自身的最终 commit SHA”。

本次采用以下可审计结构：

```text
A. production/remediation source commit
B. report update commit
C. current remote branch HEAD observed after push
```

如果本次只有 test + report 两个普通提交，可明确写：

```text
review starting HEAD: 1e537697...
final test/source commit: <SHA>
report commit: <SHA if known when subsequent receipt is made>
remote branch HEAD at final handoff: <SHA>
```

### 6.2 Required final handoff receipt

为了避免报告自引用死循环，**最终 remote HEAD 的权威回执可以放在执行代理给用户的最终交付消息中**，而报告至少必须：

1. 不再写“报告提交后将再次核对”这种未来时表述；
2. 明确区分源码/test commit 与 report-only commit；
3. 给出本次更新报告时已知的完整 `origin/main...HEAD` stat/name-status；
4. 给出 `review starting HEAD = 1e537697...`；
5. 声明 production source 是否在本任务中发生变化；正常预期应为 `No`。

最终 push 后，执行代理交付消息必须实际执行并报告：

```powershell
git fetch origin
git rev-parse HEAD
git rev-parse origin/codex/start-admission-fence-fix-20260913-003112
git status --short
git diff --stat origin/main...HEAD
git diff --name-status origin/main...HEAD
```

要求：

```text
local HEAD == remote branch HEAD
working tree clean
```

如果 local/remote 不一致，不得报告完成。

### 6.3 Current full-scope changed files expectation

最终相对 `origin/main` 的完整变更仍应围绕现有 15 个文件；本任务通常不会增加新 production/test 文件。

若出现额外 changed file，必须解释为什么属于最终验收收口所必需。

---

## 7. Report Content to Preserve

不要删掉上一报告已经形成的关键证据，保留并更新：

```text
60 normal sessions
20 whole / 20 prefix / 20 normal
post-boundary Disabled = 0
partial frame = 0
cross-session leak = 0
double release = 0
trace queue dropped = 0
trace incomplete = false
61 clean classifications including recovery
1 failed classification
0 inconclusive
0 gate-drop
0 missing join
overflow fail-closed evidence
no-callback completeStart(false) evidence
real hardware startup issue = not verified
system capture/Pktmon = not performed
```

新增一个明确字段/段落：

```text
Injected START failure conditions are enforced by test assertions: YES
```

并列出所有硬门禁值。

---

## 8. Acceptance Criteria

全部满足后才可再次提交最终验收：

1. 同一实现分支继续开发，起点确认为 `1e537697...`。
2. 未修改 `main`，未 rebase/force，未新开实现分支。
3. 本任务未修改 production START Fence/analyzer 语义；若有，必须提前 STOP AND ASK。
4. injected failure session 的 `heldCount > 0` 成为硬断言。
5. injected failure session 的 `failedDiscardCount > 0` 成为硬断言。
6. injected failure session 的 `releasedCount == 0` 成为硬断言。
7. injected failure session 的 `cardFrameCount == 0` 成为硬断言。
8. injected failure session 的 `syncFrameCount == 0` 成为硬断言。
9. `startReturnedFalse && fenceReturnedFalse` 成为硬门禁。
10. recovery session 仍必须 PASS。
11. `paimage_start_race` 通过。
12. `paimage_start_race_analyzer` 通过，且使用本次最新 artifact。
13. `paimage_start_fence_regression` 通过。
14. `paimage_start_overflow` 通过。
15. synthetic analyzer fixture 通过。
16. `paimage_network_test` 通过。
17. `paimage_core` 全部通过。
18. execution report 更新为完成时态，清楚区分 source/test commit 与 report commit。
19. 报告包含当前完整 `origin/main...HEAD` stat/name-status。
20. 最终交付消息给出实际 final local HEAD / remote HEAD，并证明二者相同、工作树 clean。
21. 不 merge 到 main；等待 ChatGPT 最终独立审查。

---

## 9. Stop-and-Ask Conditions

出现任一情况立即停止并向用户提问：

1. 失败 session 零输出硬断言无法稳定通过；
2. 为使断言通过必须修改 production Fence 逻辑；
3. 真实 race analyzer 再次出现 missing/gate-drop/inconclusive；
4. 当前远端实现分支 HEAD 已不是 `1e537697...`；
5. 必须新增或修改无关 subsystem；
6. 最终 Git history 无法通过普通 fast-forward push 完成。

报告：

```text
Observed failure
Exact assertion / metric
Whether production behavior is implicated
Minimal options
Recommended next action
```

---

## 10. Final Handoff

完成后 push 到原分支：

```text
codex/start-admission-fence-fix-20260913-003112
```

不要 merge `main`。

向用户只需报告：

```text
addendum task file
branch
review starting HEAD = 1e537697...
final local HEAD
final remote HEAD
local == remote
working tree clean
updated report path
race/failure hard-assert result
real-trace analyzer result
other focused regressions result
unresolved issues, if any
```

ChatGPT 将据此进行最终独立验收，并给出 `APPROVE` 或 `REQUEST_CHANGES`。
