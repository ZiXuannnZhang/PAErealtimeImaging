# HANDOFF — PAERealtimeImaging 网页端新会话交接

> 交接时间：2026-09-21（UTC+8）
>
> 交接源基线：`main@fb10721e07f9ea8c9e46bc308d250b788defb829`
>
> 本文状态块为 **2026-09-21 时点快照**；当前项目状态一律以 `PROJECT_STATUS.md` 为准（2026-09-25 已更正 canonical 源码树身份口径，见其第 2、5 节）。
>
> 本文件提交后，新的 canonical HEAD 会因为“仅文档变更”向前移动；新会话实际接手时始终以**最新 `origin/main`** 为唯一 canonical baseline。除本 HANDOFF 文档外，本次交接不改变 production/test source。

---

## 1. 新会话首先要知道的事

本项目已经完成上一阶段 START-admission、PhysicalRoundNormalizer、RoundIdentity、Session A/B/C/D 的实现、软件审查与当前范围实机验证收尾。

当前正式状态：

```text
CANONICAL_SOURCE_BASELINE                 = main

SESSION_A_SOFTWARE_REVIEW                 = APPROVE
SESSION_B_SOFTWARE_REVIEW                 = APPROVE
SESSION_C_SOFTWARE_REVIEW                 = APPROVE
SESSION_D_INTEGRATION_REVIEW              = APPROVE

A_B_C_D_FUNCTIONAL_HARDWARE_VALIDATION    = PASS_FOR_CURRENT_SCOPE

START_ADMISSION_SOFTWARE                  = INCLUDED_AND_APPROVED
START_ADMISSION_HARDWARE_ROOT_CAUSE       = NOT_PROVEN

FPGA_LABVIEW_EXTRA_TRIGGER_EXACT_SOURCE   = NOT_PROVEN
FULL_ROUND_TRIGGER_4007                    = FIELD_OBSERVATION_NOT_PROTOCOL_CONSTANT
```

“PASS_FOR_CURRENT_SCOPE”只表示用户已经执行的当前实机场景中没有发现与预期不一致的行为。

它**不等于**：

- 已证明额外 startup trigger 的精确 FPGA/LabVIEW 来源；
- 已证明 7 个额外 trigger 是协议固定行为；
- 已证明 4007 是固定的 physical-round trigger count；
- 已证明历史 START ingress-loss 只有一个硬件根因。

如果后续控制系统、启用端口数、LabVIEW、FPGA 或触发协议发生变化，必须重新观察现场 physical pattern。

---

## 2. 关于远端那个“提前于 main”的实验分支

远端当前存在一个 tip **ahead of `main`** 的分支。

该分支属于**另一个会话的实验工作**，不属于本次交接范围。

新会话默认规则：

```text
IGNORE_REMOTE_EXPERIMENT_BRANCH = TRUE
```

具体要求：

- 不因为它 ahead of main 就把它当成更新、更正式或更正确的 baseline；
- 不自动读取、比较、merge、rebase、cherry-pick 或引用该分支；
- 不把该分支中的源码、测试、文档或结论写入当前项目状态；
- 不把它视为“main 落后，需要追上”的证据；
- 只有用户后续**明确提示需要参考该实验分支**时，才单独检查它。

因此，新会话启动时只需：

```powershell
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/main:HANDOFF.md
```

不要先做“找最前面的 branch”之类的 baseline 推断。

---

## 3. Canonical 文档优先级

新会话应按以下顺序理解项目：

1. `PROJECT_STATUS.md`
2. `REPOSITORY_BASELINE.md`
3. `BUILD_STANDARD.md`
4. `Codex-GitHub双端联动快速上手.md`
5. 当前具体任务文档：`origin/codex/task-docs:TASKS/<task>.md`
6. `MC_410T_MultiCard/delivery/README.md`
7. 各子模块 README / docs
8. `CODEX_REPORTS/` 历史材料

本 HANDOFF 是快速接力入口，不覆盖上面更高优先级的治理文档。

历史 report / receipt / handoff / validation branch 中的 PENDING、旧路径、旧 executable、旧候选状态只保留历史语境，不能反向覆盖 current main。

---

## 4. 当前 production architecture

当前生产 UDP ingress：

```text
FPGA cards
  -> Windows UDP sockets
  -> PaimageAcquisition::SocketReceiver
  -> PaimageAcquisition::SourceCore
  -> PaimageAcquisition::HostOutput
```

输出分支：

```text
CardFrame
  -> save queue
  -> FileSaver

SyncFrame / TriggerGroup
  -> DataProcessor::deliverAssembled
  -> DisplayBuffer
  -> ImagingBypass
  -> RingBlockAssembler
  -> ImagingController
  -> ImagingSvc
  -> ring_recon_cuda
```

控制链：

```text
MainWindow
  -> NetworkController
  -> PaimageAcquisition::Backend
  -> ControlState / ControlSocket
  -> CONFIG / START / STOP
```

重要边界：

- 历史 `MultiPortReceiver -> DataProcessor -> PacketAssemblyBuffer` 仍可能保留于源码/测试，但不是当前 production UDP ingress owner；
- 18-byte ready packet 不是 CONFIG ACK；
- CONFIG 成功要求当前事务收到所有目标卡的 60-byte CONFIG ACK；
- software trace 不替代 NIC/driver/wire/FPGA 证据。

---

## 5. 已冻结的 PhysicalRound / RoundIdentity 语义

### 5.1 Round identity

```text
RoundIdentity = (measurementSession, roundGeneration)
```

它是跨：

- PhysicalRoundNormalizer；
- HostOutput；
- RingBlockAssembler；
- ImagingController / ImagingSvc；
- UI presentation；
- AutoSave / FileSaver round binding

的 ownership key。

### 5.2 startup filter

`startupFilterTriggerCount=X` 的单位是：

```text
new distinct physical trigger identity
```

不是 packet 数，也不是 card 数。

同一个 physical trigger 在多卡上共享一次 normalizer classification/cache 结果。

### 5.3 count boundary

`disableCountBoundary=false`：

- configured logical count 保持 CountBoundary/final 语义；
- 这是兼容模式。

`disableCountBoundary=true`：

- configured logical count 只作为 **realtime imaging cap**；
- logical index 在 cap 内：raw/save + realtime imaging；
- 超过 cap：raw/save 继续，但 realtime imaging 停止；
- physical round 继续直到 timeout。

不得把 configured count 重新解释成 disable 模式的 physical-round boundary。

### 5.4 active timeout

physical idle timeout 的 owner 是现有 PAimage poll chain。

核心规则：

- 不依赖 UI timer；
- 不依赖下一枚 trigger 到达；
- partial-startup round 也可 timeout；
- poll 与 classify 的竞争必须 exactly-once；
- timeout 后旧 identity 的 late sync/presentation fail closed；
- raw/save 仍保持原 round ownership。

### 5.5 timeout closure order

已冻结的关键顺序：

```text
TimeoutBoundary
 -> synchronous AutoSave round binding
 -> capture old-round presentation
 -> Ring / CUDA reset + stale barrier
 -> async PNG disk write may continue afterwards
```

不允许为了“更简单”重新引入：

- synthetic reconstruction completion；
- synthetic partial Ring block；
- 修改 `expectedBlocks` 来模拟 variable round；
- 独立 UI timeout owner；
- timeout 后旧 generation 重新进入新 round。

---

## 6. Trigger gap / UI observability

当前 production full-trigger gap owner：

```text
SourceCore per-card progression
 -> Decision::TriggerGap
 -> NetworkControllerPaimage observation sink
 -> CardStats
```

UI 冻结语义：

```text
缺失   = triggersPartial
跳号数 = missingTriggerCount
丢包   = packetsDropped
已采集 = Normalizer physical distinct count
已过滤 = Normalizer startup-filtered count
```

same-session trigger sequence reset：

- signed int16 forward/wrap 优先；
- normal forward gap 正常统计；
- small backstep 不计 gap、不 re-anchor；
- numerical back-jump >= 256 视为 same-session reset recovery，re-anchor，不产生 TriggerGap。

---

## 7. 保存与实时成像隔离

当前必须继续保持：

- save queue 与 ImagingBypass queue 是独立边界；
- imaging queue full / busy / stopping / stale / service-not-ready 不能记成 UDP ingress loss；
- imaging 丢弃不能改变已经接受 raw/save 对象的 round ownership；
- stop / timeout 时 save drain 与 realtime imaging discard/reset 要分别解释；
- partial Ring progress 不是 packet loss。

历史实现/测试材料：

```text
CODEX_REPORTS/
  acquisition-startup-history-20260907-13/
    imaging-isolation-20260911/
```

---

## 8. CUDA / Ring reconstruction 边界

production Ring path：

```text
ImagingBypass
 -> RingBlockAssembler
 -> ImagingController
 -> ImagingSvc
 -> ring_recon_cuda
```

当前 CUDA API/实现已经包含：

- per-A-line angle；
- per-A-line radius；
- multi-radius calibration；
- sector/splice；
- blend；
- layered sound speed；
- reset；
- normalized snapshot；
- state/grid query。

`src/RingRecon` 的 CPU implementation 主要是 reference / verify，不是 production realtime owner。

### ABI 铁律

如果修改 `RingReconCudaConfig` 结构：

- CUDA DLL；
- ImagingSvc；
- main executable；
- selftest；
- verify；
- viewer / import library

都必须重新核对 ABI 并按需要同步重建。

不能混用不同结构版本的 DLL 与 consumer。

---

## 9. 构建与交付

正式规则只看：

```text
BUILD_STANDARD.md
```

默认 Windows 构建入口：

```powershell
Set-Location <repo>\MC_410T_MultiCard\delivery
cmd /c build_mingw_debug.cmd
```

主要产物：

```text
PAimageReceiverDiagnostics.exe
ImagingSvc.exe
ring_svc_selftest.exe
ring_udp_replay.exe
```

正式交付必须绑定：

- exact Git SHA；
- tracked-clean state；
- final configure + build；
- BuildIdentity；
- Qt/MinGW/CMake/Ninja 实际版本；
- runtime provenance；
- dependency SHA256；
- tests/selftest evidence。

重要依赖至少记录：

```text
pa_recon_core.dll
cufft64_12.dll
ring_recon_cuda.dll
cudart64_12.dll
libzmq-v141-mt-4_3_5.dll
```

不要拿旧 candidate 的 binary/BuildIdentity 证明新的 main SHA。

---

## 10. 当前软件/硬件证据边界

上一阶段记录的主要软件 evidence：

```text
full CTest                  = PASS (45/45)
Windows MinGW Debug build   = PASS
Ring/CUDA software selftest = PASS
Session D integration       = APPROVE
```

用户随后执行了当前范围实机验证，并反馈未观察到与预期不一致，因此项目状态为：

```text
A_B_C_D_FUNCTIONAL_HARDWARE_VALIDATION = PASS_FOR_CURRENT_SCOPE
```

但后续回答中仍必须把以下三层证据分开：

```text
1. source/code correctness
2. automated tests / build / software selftest
3. real-hardware validation / root-cause attribution
```

不要用其中一层自动推出另一层。

---

## 11. 历史证据在哪里

`CODEX_REPORTS/` 已按主题整理：

```text
ring-reconstruction-history-202608/
  early Ring/CUDA/M2/M3/reference/benchmark history

acquisition-startup-history-20260907-13/
  PAimage migration
  startup diagnostics
  START admission
  system capture
  imaging/save isolation

round-identity-history-20260907-16/
  RingBlockAssembler / SHM
  PhysicalRoundNormalizer
  RoundIdentity remediation / validation

session-abcd-closeout-20260918/
  final A/B/C/D receipts
  Session D handoff
  hardware validation checklist
```

这些目录是 traceability archive，不是 current-state source of truth。

---

## 12. 其他子项目

### RadiusCalibration

`RadiusCalibration/` 是独立 MATLAB offline calibration 工具。

当前仍应注意：

- 需要真正的每通道 full-aperture calibration data；
- 不要对 raw Bscan 做 `circshift`；
- angle alignment 通过重建角度向量表达；
- 历史 testdata 不能自动当成正式 calibration truth；
- 尚未形成“8 通道正式全量校准已完成”的项目结论。

### 实时重建脚本

`实时重建脚本/` 是 MATLAB algorithm reference。

其中仍有历史默认值/历史数据路径/旧模拟语义，不是 current production protocol/config source。

### _migration_pack

`_migration_pack/` 是冻结 migration/provenance archive。

其中 hashed `HANDOFF.md` / `迁移包说明.md` 等保持原始历史内容，不要为了“更新文档”修改这些 frozen files。

---

## 13. 新阶段开发默认 Git 工作流

除非用户或新 task 明确另行指定：

```powershell
git fetch --prune origin
git switch main
git merge --ff-only origin/main
git status --short
git switch -c codex/<task-name>-<timestamp>
```

规则：

- 新功能/修复默认从 latest `origin/main` 开始；
- 不从 Session A/B/C/D historical branches 开始；
- 不从 START / PhysicalRound historical branch 开始；
- 不自动使用那个 ahead-of-main 实验分支；
- 不在 `main` 上直接做未审查实现；
- 不 force-push；
- 不隐式 reset/rebase 掉独有工作。

任务完成后：

1. implementation branch push；
2. local HEAD == remote HEAD；
3. 返回 exact SHA；
4. ChatGPT 独立审查 remote source/tests/evidence；
5. 通过后再决定是否进入 main。

---

## 14. 本地工作区

如果新会话需要处理 Codex Desktop / 本地 worktree：

- 远端 `origin/main` 是事实源；
- local `main` 只允许 fast-forward；
- 有 local-only commit / divergence 时先盘点；
- 不用 `reset --hard` / `clean` 强行“同步”；
- 保留 ignored CUDA/runtime、testdata、artifacts、hardware capture、日志和 build evidence；
- historical local branch/worktree 只有在确认无独有未推送内容后才能删。

已有“本地工作区非破坏整理”任务文档位于：

```text
origin/codex/task-docs:
TASKS/本地工作区非破坏整理_20260918-192023.md
```

但如果下一阶段只是网页端规划/审查，不需要主动执行该本地任务。

---

## 15. 新会话接手后的默认动作

如果用户没有立即指定新功能，建议只做以下 preflight：

1. 读取 latest `origin/main` 的四份 canonical 文档；
2. 记录当前 `main` exact SHA；
3. 确认用户下一阶段目标；
4. 需要写 task 时再去 `codex/task-docs`；
5. 不主动研究 ahead-of-main 实验分支；
6. 不主动重开已经收尾的 A/B/C/D 问题；
7. 不改变已冻结 round semantics，除非发现具体 blocker 或用户明确要求重新设计。

---

## 16. 一句话交接

```text
从 latest origin/main 开始下一阶段；
把 A/B/C/D 视为已收尾且当前范围实机通过；
保留“FPGA/LabVIEW 额外 startup trigger 精确来源未证明”的边界；
默认忽略远端 ahead-of-main 实验分支，除非用户以后明确要求参考。
```
