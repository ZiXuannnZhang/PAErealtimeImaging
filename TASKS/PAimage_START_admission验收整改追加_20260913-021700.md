# PAimage START admission 语义修复 — 验收整改追加任务

发布时间：2026-09-13 02:17 +08:00  
任务执行端：Codex Desktop  
执行模型：GPT-5.6 Luna  
思考强度：Max  
任务性质：**同一实现分支上的验收整改；禁止重新开实现分支**

---

## 1. Context

原任务：

```text
TASKS/PAimage_START_admission语义修复_20260913-003112.md
```

正式基线仍为：

```text
origin/main
f326056ee99e5f9c135635d7e97fe2027ee50fbc
```

当前实现分支：

```text
codex/start-admission-fence-fix-20260913-003112
```

本追加任务发布前核实的远端 HEAD：

```text
30987c183b7f7cb172ae0137d7132dea884c812d
```

上一轮独立验收结论：

```text
REQUEST_CHANGES
```

本任务不是重新设计 START Fence，而是修复验收发现的确定问题，并补齐原任务要求但当前证据不足的测试/报告。

### 1.1 分支规则

**必须继续在同一实现分支修正：**

```text
codex/start-admission-fence-fix-20260913-003112
```

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

两端 HEAD 必须均为：

```text
30987c183b7f7cb172ae0137d7132dea884c812d
```

若远端实现分支已变化，立即停止并报告；不要 reset/rebase/force。

**禁止：**

- 新开实现分支；
- 重做原任务；
- 合并 validation branch；
- 修改 `main`；
- 顺手修 receiver fairness、basePacket、partial-save、Ring/IPC、runtime deployment 等无关问题。

最终完整审查仍以：

```text
origin/main@f326056e... -> 本实现分支最终 HEAD
```

为准。

---

## 2. Review Finding A — `completeStart(false)` 可被错误覆盖为成功

### 2.1 现状

当前 `SocketReceiver::completeStart(bool ok)` 的核心逻辑：

```cpp
bool fenceOk = ok;
if (startFenceActive_) {
    if (!startFenceCallbacksSeen_)
        for (auto& state : startCardStates_)
            state = StartCardState::StartSent;

    fenceOk = !startFenceFailed_ &&
              std::all_of(startCardStates_.begin(), startCardStates_.end(),
                          [](StartCardState state) {
                              return state == StartCardState::StartSent;
                          });
}
```

这里存在明确错误：

```text
ok == false
AND
startFenceCallbacksSeen_ == false
```

时，会把所有卡人工标成 `StartSent`，随后用新的表达式覆盖 `fenceOk`，导致：

```text
completeStart(false) -> true
core_.completeStart(true)
SourceCore enabled
```

这违反原任务：

```text
任一 START/global failure 必须 fail closed
```

### 2.2 必须修复的语义

调用方传入的 `ok=false` 必须是不可逆失败条件。

推荐最小逻辑：

```cpp
bool fenceOk = ok;
if (startFenceActive_) {
    if (ok && !startFenceCallbacksSeen_) {
        // compatibility-only success path for component callers
        for (...) state = StartCardState::StartSent;
    }

    fenceOk = ok &&
              !startFenceFailed_ &&
              allCardsAreStartSent;
}
```

要求：

1. `ok=false` 时绝不能 enable SourceCore。
2. `ok=false` 时 held data 必须 discard。
3. `completeStart(false)` 必须返回 false。
4. fence state 必须正确 reset，使下一次 START 可以恢复。
5. 兼容已有 component test 的 `prepareStart(); completeStart(true);` 无 callback 成功路径。

不要通过删除 compatibility path 来让既有 component tests 大面积失效；如果认为必须删除，请先停下解释。

### 2.3 必须新增的直接回归

增加一个**无 per-card callback**的 fail-closed 测试，直接覆盖：

```text
receiver.prepareStart(sessionA)
receiver.completeStart(false)
```

至少断言：

```text
return == false
SourceCore / receiver admission remains disabled
heldReleased == 0
no CardFrame output
no SyncFrame output
```

随后发送一个采样 datagram，必须不能形成新 session frame；若它进入 SourceCore，应观察到 `Disabled` 或等价明确拒绝。

然后再执行：

```text
prepareStart(sessionB)
completeStart(true)
```

确认 compatibility success path 仍正常工作，下一 session 不受失败 session 污染。

如果更适合放入现有 `socket_replay`/独立 test，可自行选择；关键是测试必须直接覆盖 `completeStart(false)` 无 callback 分支，而不是依赖 ControlSocket failure hook。

---

## 3. Review Finding B — analyzer 对 START boundary 的解释错误

### 3.1 已选定的修复方案

**固定采用方案 B。不要新增 before-send trace schema。**

方案 B：

> analyzer 不再把 stage6 START result 时间当作“唯一且严格的 START lower boundary”。当 raw ingress 时间早于 stage6 result，但同一 `(session, correlation, card, trigger, packet)` 已存在 `StartFenceHeld` decision 时，应认定这是合法的 `StartSendPending` 窗口，而不是 pre-START missing / inconclusive。

### 3.2 为什么必须这样处理

生产顺序是：

```text
beforeStartSend(card)
    -> card state = StartSendPending
sendto(START)
    -> OS / NIC may make command visible to card
card responds immediately
receiver recvfrom(sample)
    -> stage1 raw ingress
    -> StartFenceHeld
ControlSocket sendto returns
observer emits stage6 START result
```

因此合法数据完全可能满足：

```text
rawIngressNs < stage6StartResultNs
```

而当前 analyzer 使用：

```text
stage6StartResultNs == boundary
```

并把所有 `raw < boundary` 都优先解释为 pre-start 区域。若实际 joined decision 是 `StartFenceHeld`，当前逻辑会将它错误记为 missing join / inconclusive。

### 3.3 方案 B 的精确判定要求

对每个 raw ingress，先按现有精确 key：

```text
(session, correlation, card, trigger, packet)
```

获取 stage2 decisions。

然后按 decision evidence 优先级判断，而不是单纯先按时间判断。

建议逻辑：

#### Case 1 — `StartFenceHeld` present

无论：

```text
rawNs < stage6 resultNs
或
rawNs >= stage6 resultNs
```

只要同一 ingress 的 joined decisions 包含：

```text
StartFenceHeld
```

则该 ingress 属于合法 START Fence admission：

```text
heldIngress += 1
not pre-start missing
not missingStage2Join
```

若后续同一 ingress 还有：

```text
StartFenceReleased
Accepted / Duplicate / OffsetOutside / ...
```

照常计入 release/direct decision。

#### Case 2 — `StartFencePreStartDiscard` present

同一 ingress 若包含：

```text
StartFencePreStartDiscard
```

则明确分类为 pre-card-START stale discard：

```text
preStartDiscard += 1
```

它不应因为 stage6 时间关系不同而被误标为 missing。

#### Case 3 — `Disabled` present

同一 ingress 若有 direct source decision：

```text
Disabled
```

继续认定为：

```text
application_start_gate_drop_observed
```

不要因为其时间早于 stage6 result 而降级成普通 pre-start。

#### Case 4 — 无任何可解释 decision

如果 raw ingress 无 `Held / PreStartDiscard / direct source / fence failure` 等可关联 decision，则才进入：

```text
missingStage2Join / inconclusive
```

### 3.4 stage6 时间仍保留的用途

stage6 START result 时间仍可用于：

- 每卡控制命令发送结果展示；
- `delta raw ↔ send result` 诊断；
- 识别 START send failure；
- 后续现场时序分析。

但不能再作为唯一 admission lower boundary。

如果字段名仍叫 `startSendNs`，建议在 analyzer 输出/文档中明确它实际是：

```text
START send result observation time
```

可更名为：

```text
startSendResultNs
```

若更名会破坏大量既有 consumer，可保留旧字段并新增解释字段；不要为本整改大规模改 schema。

---

## 4. Fix synthetic analyzer fixture

当前 clean synthetic fixture 不符合真实 trace 关系：它为相同 correlation 人工生成多份 stage1 raw，再分别绑定 Held / Released / Accepted。

真实 production 语义应是：

```text
ONE stage1 raw ingress
MANY stage2 observations with same correlation:
    StartFenceHeld
    StartFenceReleased
    Accepted / Complete / ...
```

### 4.1 必须修改 fixture builder

新增能够表达：

```text
1 x stage1 raw
N x stage2 decisions
```

的 helper。

### 4.2 必须新增 deterministic “raw before stage6 result” fixture

时间顺序必须明确构造：

```text
t=100  card0 raw stage1
 t=101 StartFenceHeld
 t=102 StartFenceReleased / Accepted
 t=110 card0 stage6 START result
```

注意 START result 可以在 raw 之后。

预期：

```text
classification = application_start_fence_clean
missingStage2JoinCount = 0
heldIngressCount = 1
releasedIngressCount = 1
```

同时保留：

- gate-drop fixture；
- failed fixture；
- trace incomplete fixture；
- cross-session same correlation/trigger/packet fixture。

### 4.3 pre-start stale fixture

明确构造：

```text
raw before START result
joined decision = StartFencePreStartDiscard
```

预期：

```text
preStartDiscardCount = 1
missingStage2JoinCount = 0
```

用于证明方案 B 不会把真正 stale 包误当成 Held 合法数据。

---

## 5. Strengthen real START race regression

现有 race test 主方向正确，但验收断言不够严格。必须补以下直接断言。

### 5.1 post-boundary Disabled 必须严格为 0

对成功 session：

```text
Decision::Disabled == 0
```

尤其是 whole-trigger / prefix-trigger 场景。

不能只通过 complete frame 间接证明。

### 5.2 HOLD / release 使用精确相等

对于预期进入 HOLD 的首触发 prefix：

```text
rawPre == expectedPre
heldPre == expectedPre
releasedPre == expectedPre
```

不要使用 `>=`。

这用于捕获：

- double hold；
- double release；
- 重复 observation；
- session 泄漏。

### 5.3 failure session 必须零输出

当前 failure test 只验证 START 返回失败和有 discard evidence；追加：

对 failed session `9000`（或实际使用的 failed session）：

```text
CardFrame outputs == 0
SyncFrame outputs == 0
released held packets == 0
```

并确认下一 recovery session 正常 complete。

失败 session 的 held 包只能出现：

```text
StartFenceHeld
-> StartFenceFailedDiscard
```

不得出现：

```text
StartFenceReleased
CardOutput
SyncOutput
```

### 5.4 pre-start stale trigger 不得形成 frame

现有 pre-start stale test 只计 discard。新增断言：

```text
stale trigger (e.g. 0xff00) has no CardFrame / SyncFrame output
```

### 5.5 trigger wrap / repeated session isolation

现有 60 success sessions 已覆盖多个连续 session，并使用了 `0xfffe / 0xffff / 0x0000 / 0x0001` 邻域。

保留该覆盖，并在 result/report 明确输出：

```text
session count
wrap-near session count
cross-session output leakage = 0
double release = 0
```

若当前数据结构无法直接给出这些统计，可在 test harness 内增加明确计数。

---

## 6. Add START Fence overflow regression

当前 production implementation 已有：

```text
kMaxStartHoldDatagrams = 8192
kMaxStartHoldBytes = 64 MiB
```

并声称 overflow：

```text
StartFenceOverflow
-> startFenceFailed = true
-> discardHeld
-> completeStart returns false
```

但当前验收没有直接测试该语义。

必须增加至少一个 deterministic overflow regression。

推荐做法：

1. `prepareStart(session)`；
2. 把目标 card 置为 `StartSendPending` / `StartSent`（使用公开 production callback 接口，不要直接改 private state）；
3. 经真实 loopback UDP 向该 data socket 发送超过 8192 个小合法 datagram，保证 receiver 实际进入 HOLD；
4. 等待 `StartFenceOverflow > 0`；
5. 补齐其他 card START state（若测试结构需要）；
6. 调用 `completeStart(true)`；
7. 必须得到：

```text
completeStart == false
StartFenceOverflow > 0
StartFenceFailedDiscard > 0
StartFenceReleased == 0 for overflow transaction
CardFrame == 0
SyncFrame == 0
```

之后执行一个正常 START，确认可恢复。

如果真实 UDP 发送 8193+ datagram 造成测试运行时间不可接受，可以增加**仅测试编译可见**的 hold-limit seam，但必须满足：

- production constant 和 production behavior 不变；
- seam 不出现在 delivery build；
- 报告解释测试 limit 与 production limit 的关系。

不得为了测试方便降低 production 8192/64MiB 上限。

---

## 7. Analyze the REAL race-test trace

这是本追加任务的关键验收项。

不能只测试 analyzer synthetic fixtures；必须把：

```text
paimage_start_race
```

实际产生的 trace artifact 交给：

```text
startup_diagnostics_analyze.py
```

并机器断言分类结果。

### 7.1 Required integration assertions

对 60 个正常成功 session：

```text
classification == application_start_fence_clean
missingStage2JoinCount == 0
disabledIngressCount == 0
traceIncomplete == false
```

对故障 session：

```text
classification == application_start_fence_failed
```

如果 overflow session 也包含在同一 trace：

```text
classification == application_start_fence_failed
StartFenceOverflow evidence present
```

### 7.2 CTest integration

推荐新增一个独立 CTest，例如：

```text
paimage_start_race_analyzer
```

并设置：

```text
DEPENDS paimage_start_race
```

它读取 `paimage_start_race` 生成的 artifact，而不是重新伪造 synthetic trace。

也可以采用等价方式，但必须保证 CI/本地 `ctest` 自动执行真实 trace → analyzer → assertions，不能只在 execution report 手工运行一次。

---

## 8. Preserve production scope

本追加任务允许生产代码修改范围原则上只应涉及：

```text
SocketReceiver::completeStart fail-closed bug
```

方案 B 是 analyzer 修复，不应要求新的 production trace schema。

预计主要修改：

```text
MC_410T_MultiCard/delivery/src/PaimageAcquisition/SocketReceiver.cpp
MC_410T_MultiCard/delivery/tests/paimage_start_race_test.cpp
MC_410T_MultiCard/delivery/tests/startup_diagnostics_analyze_test.py
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py
CODEX_REPORTS/start-admission-fence-fix-20260913/implementation-report.md
```

若新增一个小型 test source/script 可接受。

非必要不要再修改：

```text
Backend.cpp
ControlSocket.cpp/.h
SourceCore production assembly behavior
HostOutput / OutputWorkers
DataProcessor / FileSaver
CMake runtime deployment
ImagingSvc
Ring/IPC
README
```

如果发现必须改变原 START Fence 核心架构才能修这些问题，先停下报告，不要扩大修改。

---

## 9. Required validation

至少执行并记录 exact command/result：

### Python analyzer

```powershell
python -m py_compile MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py
python MC_410T_MultiCard/delivery/tests/startup_diagnostics_analyze_test.py
```

### START Fence tests

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release -R '^paimage_start_race$' --output-on-failure
```

如果新增 analyzer integration test：

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release -R 'paimage_start_race(_analyzer)?' --output-on-failure
```

### PAimage core

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release/paimage_core --output-on-failure
```

### Network regression

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release -R '^paimage_network_test$' --output-on-failure
```

### Production build

使用当前分支已有环境完成实际 production MinGW Debug build。

不要为 `0xc0000135` 等已知测试运行环境问题重新引入上一验证分支的 runtime deployment 改造。

若全量 CTest 仍有 baseline-existing DLL 环境失败，按原任务规则提供 baseline 对照；不要修改无关构建系统。

---

## 10. Required numeric acceptance evidence

执行报告必须给出下面数字，不能只写 PASS：

### Successful START sessions

```text
successSessionCount
wholeTriggerSessionCount
prefixTriggerSessionCount
normalAfterStartSessionCount
wrapNearSessionCount
rawIngressCount
heldIngressCount
releasedIngressCount
postBoundaryDisabledCount     MUST = 0
missingStage2JoinCount        MUST = 0
completeFrameCount
partialFrameCount             MUST = 0
syncFrameCount
crossSessionFrameLeakCount    MUST = 0
doubleReleaseCount            MUST = 0
traceQueueDropped             MUST = 0
traceIncomplete               MUST = false
```

### Failed START session

```text
startReturnedFalse = true
fenceReturnedFalse = true
heldCount
failedDiscardCount
releasedCount                 MUST = 0
cardFrameCount                MUST = 0
syncFrameCount                MUST = 0
recoverySessionPassed         MUST = true
```

### No-callback `completeStart(false)` regression

```text
completeStartFalseReturnedFalse = true
sourceRemainedDisabled = true
releasedCount = 0
cardFrameCount = 0
syncFrameCount = 0
nextCompatibilityStartPassed = true
```

### Overflow regression

```text
overflowObserved              MUST = true
completeStartReturnedFalse    MUST = true
overflowCount > 0
failedDiscardCount > 0
releasedCount                 MUST = 0
cardFrameCount                MUST = 0
syncFrameCount                MUST = 0
recoveryPassed                MUST = true
```

### Real race-trace analyzer

```text
cleanSessionCount             MUST = all normal success sessions
failedSessionCount            MUST include injected send failure
inconclusiveSuccessSessions   MUST = 0
gateDropSuccessSessions       MUST = 0
missingStage2JoinCount        MUST = 0
```

---

## 11. Update the existing execution report

不要新建第二份主报告。更新：

```text
CODEX_REPORTS/start-admission-fence-fix-20260913/implementation-report.md
```

报告必须补齐上一版缺失项。

### Git receipt

必须给出实际数值：

```text
baseline SHA
review-remediation starting SHA = 30987c183b7f7cb172ae0137d7132dea884c812d
final local HEAD
final remote HEAD
local == remote: true/false
```

以及：

```powershell
git diff --stat origin/main...HEAD
git diff --name-status origin/main...HEAD
```

完整记录输出。

### Design remediation

明确说明：

1. `completeStart(false)` 如何保持 fail closed；
2. 无 callback compatibility success 如何保留；
3. analyzer 方案 B 的 decision-first 判定规则；
4. 为什么 raw-before-stage6 + Held 是合法 pending-window；
5. 如何仍能识别真正 `PreStartDiscard`；
6. overflow regression 如何验证。

### Tests

列出本任务全部新增/修改测试及 exact command、结果、数字。

### Hardware status

仍然必须保持：

```text
software deterministic regression: verified / not verified
real hardware startup issue: not verified   # 若未实机复测
system capture/Pktmon: not performed        # 若未执行
```

不要把 loopback 修复验证升级为真实 FPGA/NIC 根因已修复。

---

## 12. Acceptance criteria for this addendum

只有全部满足才可再次提交验收：

1. 继续使用同一实现分支，没有新分支/rebase/force。
2. `completeStart(false)` 无 callback 路径严格 fail closed。
3. compatibility `completeStart(true)` 无 callback 路径仍通过。
4. analyzer 采用方案 B，没有新增 before-send production trace schema。
5. synthetic fixture 使用 1 个 stage1 + 多 stage2 的真实 correlation 模型。
6. raw-before-stage6 + `StartFenceHeld` fixture 分类 clean。
7. raw-before-stage6 + `StartFencePreStartDiscard` fixture正确计为 stale discard。
8. whole/prefix/normal 成功 session post-boundary Disabled = 0。
9. expected HOLD/release 使用精确相等断言。
10. failed START session CardFrame = 0、SyncFrame = 0、Released = 0。
11. pre-start stale trigger 无 frame 输出。
12. overflow 有 deterministic fail-closed 回归并可恢复。
13. 真实 `paimage_start_race` trace 自动跑 analyzer。
14. 所有正常 success session analyzer = `application_start_fence_clean`。
15. injected failure session analyzer = `application_start_fence_failed`。
16. success session `missingStage2JoinCount = 0`。
17. PAimage core regression 通过。
18. paimage_network_test 通过。
19. production build 完成，或若环境阻塞则提供 baseline-equivalent 证据且未扩大构建系统修改。
20. execution report 已更新到最终 remote HEAD，包含完整 Git receipt 与数字证据。
21. 不 merge 到 main；等待 ChatGPT 二次独立审查。

---

## 13. Stop-and-ask conditions

遇到以下情况先停下向用户提问：

1. 修复 `completeStart(false)` 会要求改变 ControlState 对外契约；
2. 方案 B 无法在现有 trace correlation 中区分 `Held` 与 stale packet；
3. 真实 race trace 存在无法用 `(session, correlation, card, trigger, packet)` 唯一关联的 production ambiguity；
4. overflow 无法 fail closed，除非大幅修改 Backend/ControlState API；
5. 必须新增 production before-send trace 才能实现 analyzer 正确性——这与用户已选方案 B 冲突；
6. 实现分支远端 HEAD 已不再是本任务记录的 `30987c18...`；
7. 需要修改与本整改无关的 subsystem 才能让测试通过。

停下时给出：

```text
Observed issue
Why current addendum cannot be satisfied
Minimal options
Tradeoffs
Recommended option
Affected files/API
```

---

## 14. Final handoff

完成后 push 同一分支：

```text
codex/start-admission-fence-fix-20260913-003112
```

不要 merge `main`。

最终只需向用户报告：

```text
addendum task file
branch
old remediation-start HEAD = 30987c18...
new remote HEAD
updated report path
key test summary
unresolved issues (if any)
```

ChatGPT 会重新独立读取远端 HEAD、`main...HEAD` diff、关键 production source、全部新增回归和执行报告，并给出新的 `APPROVE` / `REQUEST_CHANGES`。
