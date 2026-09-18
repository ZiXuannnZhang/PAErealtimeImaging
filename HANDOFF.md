# HANDOFF — PAERealtimeImaging current canonical state

> 更新时间：2026-09-18（UTC+8）
>
> 本文件是新会话的快速接力入口。精确状态以 `PROJECT_STATUS.md` 为准。

## 1. Start here

```text
canonical branch = main
A/B/C/D functional hardware status = PASS_FOR_CURRENT_SCOPE
FPGA/LabVIEW exact extra-trigger source = NOT_PROVEN
remote historical branches = retained for traceability
```

当前 main 已包含此前 START-admission 与 Session A/B/C/D 的 accepted source chain。
新开发应直接基于 latest `origin/main`，不再基于旧 candidate branch。

## 2. Current architecture

Production ingress：

```text
SocketReceiver -> SourceCore -> HostOutput
```

保存和实时成像分离：

```text
CardFrame -> save queue -> FileSaver
Sync/TriggerGroup -> ImagingBypass -> RingBlockAssembler -> ImagingSvc/CUDA
```

Round ownership：

```text
RoundIdentity = (measurementSession, roundGeneration)
PhysicalRoundNormalizer = startup filter / CountBoundary / TimeoutBoundary owner
```

`disableCountBoundary=true` 时 configured count 只作为 realtime imaging cap；
raw/save 继续到 physical idle timeout。

## 3. Accepted validation boundary

当前用户实机测试反馈支持：

```text
当前已执行场景未发现与预期不一致 = PASS_FOR_CURRENT_SCOPE
```

仍未证明：

- 额外 startup trigger 的精确 FPGA/LabVIEW 产生机制；
- 7/4007 是协议固定常量；
- 历史 startup ingress-loss 只有一个硬件根因。

因此后续如果控制系统/端口数/FPGA/LabVIEW 逻辑改变，应重新观察实际 physical pattern。

## 4. Documentation map

```text
PROJECT_STATUS.md
  current status / accepted scope / next action

REPOSITORY_BASELINE.md
  branch roles / merge history / local sync governance

BUILD_STANDARD.md
  exact build / runtime / packaging / hash requirements

MC_410T_MultiCard/delivery/README.md
  current production architecture

CODEX_REPORTS/
  historical task reports / diagnostics / receipts / evidence
```

历史报告中的 PENDING、旧 executable 名、旧 branch 或旧硬件假设不覆盖当前 canonical docs。

## 5. Local workspace synchronization

远端是事实源：

```powershell
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
```

local `main` 只允许 fast-forward 到 `origin/main`。如果存在 local-only commit、divergence、
dirty tracked work 或重要 ignored assets，先盘点，不用 reset/clean 强行同步。

## 6. Build

```powershell
Set-Location <repo>\MC_410T_MultiCard\delivery
cmd /c build_mingw_debug.cmd
```

正式 build 要绑定 exact canonical SHA 和 tracked-clean state。详细规则见 `BUILD_STANDARD.md`。

## 7. Historical anchors

旧分支仍保留用于追溯，包括：

- START admission；
- physical-round normalizer；
- Session A/B/C/D；
- A/B/C/D -> main integration。

这些不是后续开发默认 baseline。需要查看历史执行证据时，从 `CODEX_REPORTS/` 对应主题目录进入。
