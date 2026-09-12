# PAimage production path START admission 语义修复

发布时间：2026-09-13 00:31:12 +08:00  
任务执行端：Codex Desktop  
执行模型：GPT-5.6 Luna  
思考强度：Max  
任务性质：**生产行为修复 + 回归测试移植 + 诊断证据保留**

---

## 1. Objective

修复当前 PAimage production path 中已经被确定性验证的 START admission 缺陷：

```text
prepareStart()
  -> SourceCore enabled=false
  -> ControlSocket 逐卡发送 START
  -> 较早收到 START 的卡立即发送首触发
  -> SocketReceiver 已经 recvfrom 成功
  -> SourceCore 因全局 START transaction 尚未 complete 而返回 Disabled
  -> 首触发完全消失或前缀丢失
  -> completeStart(true) 后才重新打开 admission
```

目标不是通过延时、并行发 START、清空统计或改变测试时序来降低复现概率，而是修正生产状态机语义，使：

1. **某张卡在主机开始向该卡发送 START 后产生的有效新 session 数据，不再因为其他卡的 START 尚未发送完成而被全局 gate 丢弃。**
2. **明确属于 START 边界之前的旧 session/stale 数据不能进入新 measurement session。**
3. 所有目标卡 START 成功时，START transaction 期间暂存的有效数据必须恰好释放一次，并按新 session 正常组包。
4. 任一 START 发送失败时，transaction 必须 fail closed：本轮暂存数据不得进入输出，也不得泄漏到下一 session。
5. 修复不得改变正常 Running 阶段的 UDP 接收、组包、多卡同步、保存、显示、Ring/IPC 语义。

本任务同时要求从验证分支**选择性移植**必要的确定性 race regression 与 START admission analyzer 能力，使修复后的 main 后续具备长期回归与实机取证能力。

---

## 2. Baseline

仓库：

```text
ZiXuannnZhang/PAErealtimeImaging
```

唯一实现基线：

```text
branch: origin/main
commit: f326056ee99e5f9c135635d7e97fe2027ee50fbc
```

任务发布前已重新从 GitHub 核实该 SHA 仍为当前 `main` HEAD。

开始实施前必须执行：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch origin

git switch main
git merge --ff-only origin/main

git rev-parse HEAD
git rev-parse origin/main
git status
```

两个 SHA 必须均为：

```text
f326056ee99e5f9c135635d7e97fe2027ee50fbc
```

若 `origin/main` 已变化，或本地 `main` 无法 `--ff-only`，**立即停止任务并报告，不要 reset、rebase、force 或自行换基线。**

从该 main 新建实现分支：

```text
codex/start-admission-fence-fix-20260913-003112
```

建议命令：

```powershell
git switch -c codex/start-admission-fence-fix-20260913-003112
```

### 2.1 验证分支不是基线

上一验证分支：

```text
origin/codex/start-race-validation-20260912-205615
remote HEAD at review time:
8431476d7ff120d774d7f6161b898f76d17b8c67
```

该分支只允许作为：

- 验证证据参考；
- race test / analyzer 的选择性移植来源；
- 测试 seam 的实现参考。

**禁止：**

```text
merge validation branch
rebase onto validation branch
从 validation branch 创建实现分支
把 validation branch 整体 fast-forward/cherry-pick 到本任务分支
```

原因：验证分支还包含与本修复无关的 runtime/CMake/build-script/文档整改，不应成为生产修复历史的隐式前置依赖。

如需参考其内容，使用类似：

```powershell
git diff origin/main..origin/codex/start-race-validation-20260912-205615 -- <明确路径>
git show origin/codex/start-race-validation-20260912-205615:<明确路径>
```

然后有意识地移植必要文件/hunk。

---

## 3. Problem / Evidence

### 3.1 当前 production START 时序

当前 `Backend::startMeasurement(session)` 的核心顺序是：

```text
control_.start(
  prepare:  receiver_.prepareStart(session)
  sender:   ControlSocket::send(START, allCards)
  complete: receiver_.completeStart(ok)
)
```

`ControlState::start()` 调用：

```text
prepare()
-> sender_(startCommand(), allCards_)
-> complete(ok)
```

`ControlSocket::send()` 对目标卡按顺序逐个执行真实 `sendto()`。

`SourceCore::prepareStart()` 当前会：

```text
enabled_ = false
clear pending / active assembly / recent state
```

`SourceCore::ingest()` 在 `!enabled_` 时直接返回：

```text
Decision::Disabled
```

而 `SourceCore::completeStart(true)` 最后才：

```text
enabled_ = true
```

因此较早卡在收到自己的 START 后立即回首触发时，数据可能已经被应用 `recvfrom()`，但仍在全局 START transaction 窗口内，被软件主动判成 `Disabled`。

### 3.2 已完成的确定性验证

上一验证任务使用真实 WinSock loopback，实际经过：

```text
ControlState
-> real ControlSocket::sendto
-> emulated card receives its own START
-> card immediately sends acquisition UDP
-> real SocketReceiver thread
-> SourceCore
-> HostOutput callbacks / trace
```

60 个 session（3 场景 × 20 次）全部按预期完成；增强 analyzer 对应用 trace 得到：

```text
startSendCount=240
rawIngressCount=3840
matchedDirectDecisionCount=3840
missingStage2JoinCount=0
disabledIngressCount=880
acceptedIngressCount=2960
traceIncomplete=false
classification=application_start_gate_drop_observed
```

验证分支的核心 race 场景：

1. `whole_trigger_gated`：首触发全部在 transaction 未 complete 时进入；raw ingress 存在，但全部 `Disabled`，下一个触发成为首个可见触发。
2. `prefix_gated_partial`：同一首触发前缀在 gate 内 `Disabled`，后缀在 complete 后被接受，产生 partial/incomplete。
3. `normal_after_start`：全部数据在 global complete 后发送，正常 complete。

该验证证明的是：**当前 production 状态机具有确定性的软件丢包机制。**

它不证明实机现场的全部启动异常都只来自这一机制；修复后仍需实机复测并继续保留 stage1/Pktmon 证据链。

---

## 4. Required Semantic Invariants

以下是本任务最重要的验收约束。实现方式可以调整，但这些语义不能被绕开。

### 4.1 新 session 隔离

`prepareStart(newSession)` 必须建立新的 measurement session 边界，并清理上一 session 中：

- active card assembly；
- pending sync；
- recent trigger state；
- 上一轮 START hold；
- 任何不能安全归属到 newSession 的残留。

上一 session 的数据不得被错误标记为 newSession。

### 4.2 START transaction 期间不得继续使用“全局 Disabled 丢弃有效新数据”语义

在 `prepareStart()` 到 `completeStart(true)` 之间，不能再简单地把所有已经进入应用的数据都送到 `SourceCore` 后得到 `Disabled`。

生产路径需要一个明确的 **START Fence / HOLD** 状态。

### 4.3 必须有 per-card START boundary

不能只用一个 global `START begin` 时间作为所有卡的 admission 边界。

原因：START 是逐卡发送；card0 可能已经收到 START 并开始发新 session，而 card3 还没发送 START。

每张卡必须至少区分：

```text
AwaitingStart        尚未开始对本卡发送 START
StartSendPending     本卡 START send 正在发生 / host boundary 已建立
StartSent            本卡 START send 已成功
StartFailed          本卡 START send 失败
```

名称可不同，但必须具备等价语义。

### 4.4 START 成功后 HOLD 数据只释放一次

当全部 required card START 成功时：

- 新 session 才能进入 Running；
- transaction 期间属于新 session 的 held datagram 必须恰好 replay/ingest 一次；
- 不得产生第二份 stage1 raw ingress；
- 原始 `ingressId`、card、source IPv4、packet/trigger 信息必须保持可关联；
- 建议保留原始 ingress monotonic timestamp，而不是把 release 时刻伪造成网络到达时刻；若实现认为必须改用 release time，先停止并说明理由/影响。

### 4.5 START 失败必须 fail closed

任一目标卡 START send 失败时：

- 本 session 不得被视为正常 Running；
- 已 held 的数据必须被明确 discard；
- 不得输出 CardFrame / SyncFrame；
- 不得进入保存、显示、Ring/IPC；
- 下一次 START 必须从干净状态重新开始；
- 必须有可观测 decision/counter/trace，不能静默丢弃。

### 4.6 pre-START stale 数据不得进入新 session

对于一张尚未建立本卡 START boundary 的卡：

- 已经到达应用的旧数据不能进入新 session assembly；
- 必须显式 discard，并可从 trace/decision 中识别为 START fence 前数据，而不是混成普通 runtime drop。

### 4.7 Running path 不退化

一旦 START transaction 成功完成，正常采集路径不应额外经过长期 hold、全局 mutex 或昂贵复制。

START Fence 应是 session transition 的短生命周期机制，不是正常 steady-state datapath 的新队列层。

---

## 5. Recommended Design

推荐采用 **SocketReceiver-owned START Fence HOLD**，让 raw datagram 在进入 `SourceCore` 前完成 START transaction 隔离。

不建议把 hold 塞进 `SourceCore` assembly，因为：

- SourceCore 当前 `enabled_` 语义清晰；
- raw ingress 已经在 SocketReceiver 层掌握 `ingressId` / source / receive timestamp；
- START boundary 与 control send 是 transport/session admission 问题，不应伪装成正常 trigger assembly。

### 5.1 推荐状态流

```text
Idle / Disarmed
   |
   | prepareStart(session)
   v
StartFenceHold
   |- card0 AwaitingStart
   |- card1 AwaitingStart
   |- ...

per card:
   before START send(card)
       -> establish host-side card START boundary
       -> StartSendPending

   sendto(START)

   after send result(card)
       success -> StartSent
       failure -> StartFailed

while StartFenceHold:
   pre-boundary ingress
       -> explicit pre-start discard

   post-boundary / send-pending / started ingress
       -> bounded HOLD with original ingress metadata

all START success
   -> completeStart(true)
   -> enable SourceCore
   -> replay held datagrams exactly once
   -> Running

any START/global failure
   -> discard all held datagrams with explicit evidence
   -> remain disarmed / non-running
```

### 5.2 为什么建议在 `sendto()` 前建立 host-side boundary

上一验证分支的 test hook 是在真实 `sendto()` 返回后才执行；这足以复现旧 bug，但正式修复不能简单依赖“sendto 返回后才把 card 标记为 started”。

如果 receiver 与 control sender 并发运行，真实硬件响应可能在：

```text
sendto 已经把 START 交给协议栈/链路
但 control thread 尚未来得及执行 after-send callback
```

之间进入 `recvfrom()`。

如果 production gate 直到 after-send callback 才允许 hold，这里仍可能留下一个更窄但真实的 race。

因此推荐增加明确的 `beforeStartSend(card)` + `afterStartSend(card,result)` 协调：

- `beforeStartSend`：本卡进入 `StartSendPending`；
- 在 `StartSendPending` 期间收到的数据先 provisional hold；
- send 成功：这些 provisional 数据保留；
- send 失败：这些 provisional 数据全部 discard。

这一定义优先保证“合法 START 响应不会因为 sender/receiver 调度顺序而丢失”。

**重要：** 当前 UDP 数据协议没有 measurement-session token，也没有 START ACK 可用于绝对区分“send syscall 期间仍在飞的旧数据”与“真正由 START 触发的新数据”。如果你在审查当前硬件/协议实现后认为 `beforeStartSend` 边界会引入不可接受的 stale admission，且无法同时满足“零新数据丢失 + 零旧数据混入”，请在修改生产语义前停止并向用户报告：

1. 你确认的不可判定窗口；
2. 两个或更多可行实现方案；
3. 每个方案是偏向 fail-open 还是 fail-closed；
4. 是否需要 FPGA/协议增加 session/START ACK 才能完全消除歧义。

不要自行选择一个会改变数据语义的折衷。

### 5.3 ControlSocket 协调

当前 `ControlSocket::send()` 已经逐卡循环，并在每个 `sendto()` 后生成 `SendResult` / observer。

可考虑扩展为显式：

```text
before-send callback(card, command)
sendto(...)
after-send callback(command, SendResult)
```

或实现等价的 transaction callback。

要求：

- CONFIG / STOP 行为保持原样；
- START 才触发 receiver START fence per-card transition；
- stage6 START trace 仍保留；
- 不得用 sleep 模拟同步；
- 不得把控制 socket 与数据 socket 错误耦合成长时间锁。

### 5.4 HOLD 数据结构

Held datagram 至少应保留：

```text
card
payload bytes / exact received length
ingressId
original receive monotonic timestamp
source IPv4
必要的 source port / trace correlation metadata
```

要求：

- 有界；
- 不允许无上限增长；
- START transaction 内操作应近似 O(1) append；
- replay 顺序必须确定性。

建议按原始 `ingressId` 全局顺序 replay；如果你选择 per-card replay，必须证明不会改变当前 SourceCore sync/trigger close 语义，并在报告中说明。

### 5.5 HOLD overflow

禁止 silent overflow。

如果 buffer 满，应至少：

- 记录明确 overflow decision/counter；
- 本 START transaction fail closed 或进入等价安全状态；
- 不把“只丢一部分 held 包然后继续 Running”当作成功。

但 **buffer 上限如何计算、overflow 如何向 `ControlState::start()` / UI 的 success 结果传播** 可能涉及公开生产语义。

如果当前结构无法在不大改 API 的情况下做到“overflow => START 失败且调用方可见”，请在实现该折衷前停止并向用户提问。不要静默选择一个返回 true 但实际已经丢 held 数据的方案。

---

## 6. Explicitly Prohibited Fixes

以下方案不能作为本任务的根修复：

1. `completeStart(true)` 提前到发送 START 之前。
2. 在 `prepareStart()` 里直接 `enabled_=true`。
3. 全局 sleep / startup delay / arbitrary idle wait。
4. 仅把 START 改成并行发送来缩短窗口。
5. 启动前简单 `Sleep()` 后清 socket。
6. 仅扩大 `SO_RCVBUF`。
7. 把 `Decision::Disabled` 统计隐藏掉但仍实际丢包。
8. 修改测试使其不再在 START transaction 内发包。
9. 用 analyzer 分类变化代替真实数据路径修复。
10. 顺手重构单线程 `SocketReceiver` fairness、first-arrival base、partial save、Ring/IPC 等其他问题。

---

## 7. Selective Port From Validation Branch

本任务需要长期保留“修复前必现 / 修复后不再丢”的回归能力，因此要求从：

```text
origin/codex/start-race-validation-20260912-205615
```

选择性移植以下能力，允许根据新修复结构调整：

### Required

```text
MC_410T_MultiCard/delivery/tests/paimage_start_race_test.cpp
MC_410T_MultiCard/delivery/tests/startup_diagnostics_analyze_test.py
MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py
MC_410T_MultiCard/delivery/tests/CMakeLists.txt 中与上述测试直接相关的最小部分
ControlSocket test seam 中仍有必要的最小部分
```

### Do NOT port unless independently required and explicitly justified

```text
runtime dependency / IMAGING_RUNTIME_DIR 整改
build_mingw_debug.cmd 的无关改动
ImagingSvc CMake 的无关改动
_migration_pack 的无关改动
执行代理快速上手指南.md
验证报告 commit 本身
其他与 START admission fix 无关的文档/构建变化
```

移植后 race test 必须由“验证旧 bug”改为“验证新语义”。

---

## 8. Required Tests

### 8.1 Deterministic START race regression — whole trigger

4 张逻辑卡；每张卡收到自己的 START 后立即发首触发完整数据，而 host 仍在向后续卡发送 START。

修复后必须满足：

```text
raw ingress: present
post-boundary Disabled: 0
first trigger: complete
next trigger: complete
no trigger jump caused by START gate
all cards can form expected sync frame
held datagrams released exactly once
```

至少 20 次 session 重复。

### 8.2 Prefix-before-global-complete + suffix-after-complete

每卡首触发：

```text
prefix -> transaction 尚未 global complete
suffix -> global complete 后
```

修复后同一 trigger 必须成为**一个完整 frame**。

不得出现旧验证中的：

```text
prefix Disabled
suffix starts a shifted assembly
partial first trigger
```

至少 20 次 session 重复。

### 8.3 Normal-after-start

所有采样 UDP 均在 global complete 后发送。

用于确认普通路径行为不退化。

至少 20 次 session 重复。

### 8.4 Pre-card-START stale ingress

构造明确早于该 card START boundary 的数据。

要求：

- raw ingress 可以存在；
- 不得进入新 session SourceCore assembly；
- 不得输出 frame；
- 有明确 `StartFencePreStartDiscard` 或等价 decision/evidence；
- 后续真正 START 数据仍可正常 complete。

### 8.5 START send failure

至少覆盖：

```text
card0 success
card1 success
card2 send failure
card3 是否尝试按当前 ControlSocket 语义处理由实现保持一致
```

要求：

- START API/状态不得进入正常 Running；
- card0/card1 已 held 数据不得输出；
- held discard 有明确 evidence；
- 下一次新的成功 START 不受污染；
- 不允许上一失败 session 数据泄漏到下一 session。

如果测试环境无法通过真实 UDP `sendto` 稳定制造失败，可以增加**仅测试编译可见**的最小 failure seam，但必须保证 production build 不含行为分支。

### 8.6 Repeated session isolation

连续至少 50 轮：

```text
prepare -> per-card START -> hold -> release -> running -> stop/next prepare
```

使用不同 triggerSeq / session id，至少包含一次 triggerSeq wrap-around 邻近值（例如 `0xfffe`, `0xffff`, `0x0000`, `0x0001`），确认 session boundary 不依赖 triggerSeq 单调跨轮次。

要求：

- no cross-session held packet leakage；
- no double release；
- no stale recent-trigger false rejection from previous session；
- no trace queue drop in test fixture；
- no deadlock / timeout。

### 8.7 Analyzer regression

保留并更新 synthetic fixtures，至少覆盖：

- fixed whole-trigger race：post-boundary no Disabled；
- fixed prefix race：single complete frame；
- pre-start discard evidence；
- START send failure / held discard；
- trace incomplete / queueDropped 不能误判 clean；
- session/trigger/packet 重号不能 cross-session 误 join。

Analyzer 分类建议新增或更新为能够区分：

```text
application_start_gate_drop_observed      # 旧缺陷 / 若再次出现
application_start_fence_clean             # START 边界证据完整且无 post-boundary gate drop
application_start_fence_failed            # send failure / hold abort / overflow 等
application_start_fence_inconclusive      # trace 不完整或缺 correlation
```

具体字段名可调整，但必须机器可读、逐 session/card 可追溯。

---

## 9. Regression Baseline

除新增 START fence 行为外，以下既有语义不得变化：

- CONFIG ACK 卡发现逻辑；
- CONFIG / STOP command bytes；
- feedback socket 行为；
- Running 状态下 SourceCore trigger assembly；
- Duplicate / OffsetOutside / RecentTrigger 等既有 decision；
- startupPolicy `bypass` 默认语义；
- legacy startup filter 只作为历史/回归比较，不作为本修复；
- HostOutput / FrameConverter / DataProcessor 接口；
- save/display/ring routing；
- SocketReceiver steady-state select/drain 策略；
- 诊断 stage1 raw ingress 仍必须发生在 admission/dedup 之前。

不要在本任务中修：

- single receiver drain fairness；
- first-arrival packetSeq 作为 assembly base；
- incomplete CardFrame 可进入保存；
- downstream sync queue block eviction；
- LoopLog fixture 历史问题；
- Qt/runtime bundle 构建环境整改。

---

## 10. Expected Changed Files

实际文件可有小范围调整，但预计主要集中：

```text
MC_410T_MultiCard/delivery/include/PaimageAcquisition/SocketReceiver.h
MC_410T_MultiCard/delivery/src/PaimageAcquisition/SocketReceiver.cpp
MC_410T_MultiCard/delivery/include/PaimageAcquisition/ControlSocket.h
MC_410T_MultiCard/delivery/src/PaimageAcquisition/ControlSocket.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/Backend.cpp
MC_410T_MultiCard/delivery/include/PaimageAcquisition/SourceCore.h   # only if new decisions/counters needed
MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp   # only if observation plumbing requires
MC_410T_MultiCard/delivery/tests/paimage_start_race_test.cpp
MC_410T_MultiCard/delivery/tests/startup_diagnostics_analyze_test.py
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py
```

如需要修改明显超出该列表的 production subsystem，先判断是否为修复必需；若会改变外部行为或扩大架构范围，停止并向用户说明原因。

---

## 11. Build / Test Commands

先确认本机已有 Qt/MinGW/CUDA runtime 环境；**不要因为本任务重新引入验证分支中那些无关 runtime/CMake 整改。**

如果当前 main 在该工作站因外部 runtime bundle 缺失无法完整 build，而你无法使用已有本地 bundle 解决，请停止并报告环境阻塞，不要顺手修改构建系统。

至少执行：

```powershell
python -m py_compile MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py
python MC_410T_MultiCard/delivery/tests/startup_diagnostics_analyze_test.py
```

配置/构建测试目录后：

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release -R '^paimage_start_race$' --output-on-failure
```

要求 race test 内部完成各场景规定的重复次数，而不是只运行一次单 session。

同时执行 PAimage core regression：

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/paimage_core --output-on-failure
```

以及可用环境下的全量 tests_release：

```powershell
ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release --output-on-failure
```

如果有预先存在、与本任务无关的测试失败：

- 单独复跑；
- 证明失败在 baseline main 上同样存在，或提供明确历史依据；
- 不得为了“全绿”修改无关模块。

生产目标至少需要完成实际 MinGW Debug configure/build；报告 exact command、环境变量/runtime bundle 来源以及结果。

---

## 12. Acceptance Criteria

全部满足才可报告任务完成：

1. 实现分支严格从 `main@f326056e...` 创建。
2. 未 merge/rebase/cherry-pick 整个验证分支。
3. START Fence 属于 production path，而不是仅测试 workaround。
4. per-card START boundary 存在并有明确状态机。
5. 有效 post-boundary ingress 不再因 global START 未 complete 而 `Disabled`。
6. whole-trigger race 20+ sessions：首触发不跳失，全部 complete。
7. prefix race 20+ sessions：首触发形成单一 complete frame，不再 partial。
8. normal-after-start 20+ sessions：无回归。
9. pre-card-START stale ingress 不进入新 session。
10. START send failure fail closed，held 数据不输出、不跨 session 泄漏。
11. repeated session isolation 50+ rounds，无 double release / deadlock / timeout。
12. race test 无 socket hard error、emulator send error、trace queue drop。
13. stage1 raw ingress 仍只记录一次，replay 不伪造第二次 raw ingress。
14. analyzer 可区分 clean / gate-drop / failed / inconclusive，且 exact join 不跨 session。
15. Running steady-state datapath 不增加长期 hold。
16. 不顺手修改 receiver fairness / basePacket / save policy / Ring/IPC。
17. 构建与测试实际命令、结果、失败项均有完整记录。
18. 提交前 `git diff origin/main...HEAD` 只包含本任务必需修改。
19. 分支已 push，local HEAD == remote branch HEAD。
20. 未 merge 到 main、未 force push。

---

## 13. Stop-and-Ask Conditions

用户已明确允许在不确定选择点暂停提问。遇到下列任一情况，应停止生产语义修改并报告，不要自行猜：

1. 无法同时满足“合法新数据不丢”和“旧 session 数据不混入”，根因是协议缺少 session token / START ACK。
2. HOLD overflow 需要改变 `startMeasurement()` 对外 success/failure 契约，但当前接口无法安全传播。
3. 必须改变 CONFIG/STOP 协议或 FPGA 端行为才能完成修复。
4. 必须大幅重构 `SocketReceiver` steady-state receive architecture 才能实现 START Fence。
5. 发现验证结论依赖 validation-only 行为，当前 main production path 实际不具备相同 race。
6. 当前 `origin/main` 不再是任务指定 SHA。
7. 构建环境阻塞只能通过引入与本任务无关的大范围 CMake/runtime 改造解决。

报告时给出：

```text
Observed constraint
Why current invariant cannot all be satisfied
Option A + tradeoff
Option B + tradeoff
Recommended choice
Files/API affected
```

---

## 14. Required Execution Report

提交：

```text
CODEX_REPORTS/start-admission-fence-fix-20260913/implementation-report.md
```

报告至少包含：

### Git

- task document filename；
- baseline branch + exact baseline SHA；
- implementation branch；
- final local SHA；
- final remote SHA；
- local == remote 核验；
- `git diff --stat origin/main...HEAD`；
- changed files 完整列表。

### Design

- 最终 START Fence 状态机；
- per-card boundary 定义；
- before-send / after-send 协调方式；
- held datagram 数据结构；
- replay 顺序；
- failure rollback；
- overflow 策略；
- 为什么不会 double stage1；
- 为什么不会跨 session 泄漏。

### Validation

对每个必测场景给出数字，不接受只写 `PASS`：

```text
sessions
START sends
raw ingress
held ingress
released ingress
pre-start discarded
disabled post-boundary ingress
complete frames
partial frames
sync frames
held discard on failure
missing joins
trace queue drops
socket errors
timeouts
```

### Build / Test

- exact commands；
- exit codes / pass counts；
- production build 结果；
- PAimage core 结果；
- tests_release 结果；
- analyzer fixtures 结果；
- 所有未通过项是否为 baseline-existing 的证据。

### Scope audit

明确说明是否修改了：

```text
receiver fairness
SourceCore basePacket semantics
partial-save policy
Ring/IPC
CONFIG discovery
runtime deployment/build system
```

正常情况应为 `No`；若不是，必须解释为何属于本任务必要范围。

### Hardware status

本任务自动化通过也不能声称真实硬件现场已修复。

报告必须明确：

```text
software deterministic regression: verified / not verified
real hardware startup issue: verified / not verified
system capture/Pktmon: performed / not performed
```

如果没有真实采集卡复测，写 `real hardware startup issue: not verified`。

---

## 15. Handoff After Completion

完成并 push 后，不要 merge 到 main。

只向用户报告：

```text
task file
branch
remote HEAD
report path
high-level test result
任何 stop-and-ask / unresolved semantic issue
```

ChatGPT 将重新从 GitHub 独立读取：

```text
remote HEAD
baseline -> HEAD diff
production source
race tests
analyzer
execution report
```

并给出 `APPROVE` 或 `REQUEST_CHANGES`。
