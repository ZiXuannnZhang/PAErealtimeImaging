# PAERealtimeImaging

> 2026-10-19 联合候选：本实现分支 `codex/ring-timevar-lowpass-20261016` 在已审核
> `eed98f0` 上新增 FilteredDAS，保留时变低通及启动拒绝修复，旧双极/低频补偿已移除。
> 未合并 main；两增强的实机视觉、真实采集与 T_arr 待用户联合验收。
> 任务、测试、性能和 exact-SHA 联合包入口见 `CODEX_REPORTS/ring-filtered-das-20261019/README.md`。


Windows 多卡实时光声采集与成像项目。当前正式源码与开发基线均为 canonical `main`。

## Current status

截至 2026-10-13：main 是下一步开发基准。用户已确认把环形增强 bd69785 的完整源码、
测试与证据线性接入旧 main 6ff0e02，使用普通单父提交与普通快进推送，保留原分支历史。
main 的生产/测试源码与 bd69785 完全一致；旧 tip 不因此成为 main 的祖先。

当前 main 已含前端链、环形信号增强、FFT 优化、圆形显示掩膜与毫米坐标、发现早退 A 范围、
count-boundary 前端刷新解耦。A/B/C/D 当前范围实机通过；发现早退 A 与 count-boundary
于 2026-10-01 实机通过。双极补偿效果调优搁置，Layer 2b T_arr 未执行，discovery B 待决定。
7 / 4007 是现场观察而非协议常量，额外 trigger 精确 FPGA/LabVIEW 来源仍未证明。

已有轻量包实际构建于 3c6323d，其源码与 aa9097e、bd69785 相同；本次接入不产生新二进制。
构建身份、验收边界与追溯点见 PROJECT_STATUS.md。
旧副本与可再生产物按用户授权移入 _pending_delete 待审核；最终删除仅由用户执行。

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

## 2026-10-16 实现候选（尚未合入 main）

`codex/ring-timevar-lowpass-20261016` 从批准基线 a8c15cff 移除旧双极/低频增强，接入仅 Ring 服务消费的 STFT 时变低通。
默认关闭，45/1 MHz、780/2000；执行位置在完整 A-line 的 DBR/限幅/DelayCut 前。
新配置、主/高级草稿、默认保存和启动恢复已接线；独立 MATLAB 数值与软件回执见
`CODEX_REPORTS/ring-timevar-lowpass-20261016/`，模块说明见 `MC_410T_MultiCard/delivery/docs/环形重建增强.md`。
这是实现分支候选，不更改 canonical main 接受状态；硬件/视觉验收与真实 T_arr 仍未验证。
