# PAERealtimeImaging

Windows 多卡实时光声采集与成像项目。当前正式源码与开发基线均为 canonical `main`。

## Current status

截至 2026-11-04，用户确认本地最新交付包及本分支此前工作均已通过实机测试，并授权将
`codex/ring-lowpass-motor-linkage-20261029` 普通 fast-forward 接入 `main`。
已验收源码为 `1fb9478e71c95815671c5e1a2c1b3d6a302746d1`；本次只追加治理文档，不修改已验收源码。

当前能力包括原采集/前端/轮次链、圆形掩膜与毫米坐标、发现早退 A、count-boundary 前端刷新解耦，
以及 Ring STFT 时变低通、FilteredDAS、电机方向/步数参数联动、参数文件选择与持久化、物理轮内冻结及方向映射修正。
旧双极/低频补偿已移除；两项新滤波默认关闭。面板“逆”映射 `sectorCcw=0`、起点 180°，
“顺”映射 `sectorCcw=1`、起点 0°；桥接配置保持 `DirectionInverted=false`。

验收包保留 `1fb9478` 的 BuildIdentity；本次文档提交不产生新二进制。
Layer 2b T_arr 未取得独立实测记录，discovery B 待决定；7 / 4007 仍是现场观察而非协议常量，
额外 trigger 的精确 FPGA/LabVIEW 来源未证明。验收追溯见 `CODEX_REPORTS/main-acceptance-20261104/README.md`。

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
