# PAERealtimeImaging

Windows 多卡实时光声采集与成像项目。当前正式源码与开发基线均为 canonical `main`。

## Current status

用户于 2026-11-07 确认此前未验收的环形实时成像工作通过实机验收。
已验收源码及测试包 BuildIdentity 为 `4c83ecf7b0dc0e0be8ca901ca952b7521f79393a`。实现与验收文档通过
`codex/ring-realtime-performance-20261104` 发布；canonical 开发基线仍是 `origin/main`。

本轮包括同心圆折射精确核 FBP、分段和融合 GPU 预处理、按轮次和触发序号有界多卡配对、
顺逆九点钟起步，以及方向联动按物理轮生效、纯换向不重启成像服务。退役旧 FilteredDAS 和可选
DAS 权重控件；时变低通仍可用。实机验收按用户测试范围记录，八通道性能专项仍暂缓。
详见 [验收记录](CODEX_REPORTS/ring-realtime-acceptance-20261107/README.md)。

## Canonical documentation

按以下顺序读取（**权威顺序**以 `Codex-GitHub双端联动快速上手.md` §1 为准，两处清单同一套治理文档）：

1. `PROJECT_STATUS.md` — 当前项目与验证状态；
2. `REPOSITORY_BASELINE.md` — branch / history / merge governance；
3. `BUILD_STANDARD.md` — Windows build、runtime provenance、delivery；
4. `HANDOFF.md` — 简洁的当前接力入口，不替代上述三份治理文档；
5. `Codex-GitHub双端联动快速上手.md` — ChatGPT / Codex 协作方式；
6. `MC_410T_MultiCard/delivery/README.md` — 当前生产采集/实时成像架构；
7. `CODEX_REPORTS/README.md` — 历史执行、验证与诊断档案说明。

任务特殊要求以 `codex/task-docs:TASKS/<task>.md` 为准，可覆盖上述治理文档中与之冲突的条款。

## Production data path

```text
FPGA cards
 -> PaimageAcquisition::SocketReceiver
 -> SourceCore
 -> HostOutput
      -> CardFrame -> FileSaver
      -> SyncFrame -> DataProcessor::deliverAssembled
           -> FramePublisher (raw group, optional)
           -> FrontendPreprocessor (async clone / filter / display preparation)
                -> DisplayBuffer
                -> Ring feed -> RingBlockAssembler
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
