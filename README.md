# PAERealtimeImaging

Windows 多卡实时光声采集与成像项目。当前正式源码与开发基线均为 canonical `main`。

## Current status

截至 2026-09-18：

```text
canonical source                         = main
Session A/B/C software review            = APPROVE
Session D integration review             = APPROVE
A/B/C/D functional hardware validation   = PASS_FOR_CURRENT_SCOPE
exact FPGA/LabVIEW extra-trigger source  = NOT_PROVEN
7 / 4007                                 = field observation, not protocol constant
```

本轮 START-admission、PhysicalRoundNormalizer、RoundIdentity、variable-length timeout、
realtime imaging cap、AutoSave binding、Ring/CUDA reset/stale barrier 已进入 canonical source。

“当前验证范围通过”表示已执行的实机场景未发现与预期不一致，不表示已经证明额外 startup trigger
的底层 FPGA/LabVIEW 精确产生机制。

## Canonical documentation

按以下顺序读取：

1. `PROJECT_STATUS.md` — 当前项目与验证状态；
2. `REPOSITORY_BASELINE.md` — branch / history / merge governance；
3. `BUILD_STANDARD.md` — Windows build、runtime provenance、delivery；
4. `Codex-GitHub双端联动快速上手.md` — ChatGPT / Codex 协作方式；
5. `MC_410T_MultiCard/delivery/README.md` — 当前生产采集/实时成像架构；
6. `CODEX_REPORTS/README.md` — 历史执行、验证与诊断档案说明。

根 `HANDOFF.md` 是简洁的当前接力入口，不替代上述治理文档。

## Production data path

```text
FPGA cards
 -> PaimageAcquisition::SocketReceiver
 -> SourceCore
 -> HostOutput
      -> CardFrame -> FileSaver
      -> SyncFrame -> display / publisher
      -> ImagingBypass -> RingBlockAssembler
           -> ImagingController -> ImagingSvc -> ring_recon_cuda
```

旧 `MultiPortReceiver / PacketAssemblyBuffer` 代码仍可用于兼容、工具或测试，但不是 current
production UDP ingress owner。

## Round model

```text
RoundIdentity = (measurementSession, roundGeneration)
```

核心行为：

- 每轮前 `startupFilterTriggerCount` 个 new distinct physical identities 可被过滤；
- 多卡同一 physical trigger 共享分类；
- `disableCountBoundary=false`：configured logical count 是 CountBoundary；
- `disableCountBoundary=true`：configured count 只是 realtime imaging cap，raw/save 继续到 timeout；
- physical idle timeout 由 PAimage poll chain 单一拥有；
- timeout closure 保证 save binding、old presentation capture、Ring/CUDA reset 与 stale rejection；
- variable/early round 不伪造 reconstruction completion。

## Build

正式 Windows 构建：

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

依赖来源、SHA256、delivery staging 和 BuildIdentity 规则只以 `BUILD_STANDARD.md` 为准。

## Repository layout

- `MC_410T_MultiCard/delivery/` — production application / ImagingSvc / Ring reconstruction；
- `PALiveImagingSimSender/` — loopback simulator；
- `RadiusCalibration/` — MATLAB offline radius calibration；
- `实时重建脚本/` — historical MATLAB algorithm reference；
- `CODEX_REPORTS/` — engineering history/evidence archive；
- `docs/history/` — repository migration history；
- `_migration_pack/` — frozen migration/provenance package。

## New development

新任务默认从 latest `origin/main` 开始：

```powershell
git fetch --prune origin
git switch main
git merge --ff-only origin/main
git switch -c codex/<task-name>-<timestamp>
```

除非 task document 明确指定 reviewed baseline，否则不要从 Session A/B/C/D、START、
physical-round 或 integration historical branches 开始新开发。
