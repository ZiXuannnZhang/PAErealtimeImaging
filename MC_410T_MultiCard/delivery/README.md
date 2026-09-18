# PAimageReceiverDiagnostics — 当前生产采集与实时成像说明

> 当前项目状态以仓库根目录 `PROJECT_STATUS.md` 为准；构建/交付以 `BUILD_STANDARD.md` 为准。
> 本 README 只描述 `MC_410T_MultiCard/delivery` 在 canonical `main` 中的当前生产架构。

## 1. 当前状态

截至 2026-09-18：

- canonical source baseline = latest `main`；
- START-admission 软件修复已包含在 canonical ancestry；
- Session A/B/C/D 的 PhysicalRound / RoundIdentity / timeout / realtime-cap 工作已集成；
- 用户实机验证当前范围内未发现与预期不一致，正式状态为
  `PASS_FOR_CURRENT_SCOPE`；
- 底层额外 startup trigger 的精确 FPGA/LabVIEW 来源仍未证明；
- “7 / 4007”是当前现场观察与操作配置，不是协议常量。

## 2. 当前生产采集链

```text
FPGA cards
  -> Windows UDP sockets
  -> PaimageAcquisition::SocketReceiver
  -> PaimageAcquisition::SourceCore
       measurement/session admission
       per-card trigger assembly
       TriggerGap / partial diagnostics
  -> PaimageAcquisition::HostOutput
       -> CardFrame -> save queue -> FileSaver
       -> SyncFrame -> DataProcessor::deliverAssembled
            -> DisplayBuffer
            -> ImagingBypass -> RingBlockAssembler -> ImagingController/ImagingSvc
            -> FramePublisher (optional)
```

历史 `MultiPortReceiver -> DataProcessor -> PacketAssemblyBuffer` 仍保留在源码/测试中，
但不是当前生产 UDP ingress owner。

控制链：

```text
MainWindow
  -> NetworkController
  -> PaimageAcquisition::Backend
  -> ControlState / ControlSocket
  -> CONFIG / START / STOP
```

CONFIG 成功要求当前事务收到全部目标卡 60 字节 CONFIG ACK；18 字节 ready packet
只表示设备就绪，不替代配置确认。

## 3. START admission

当前 canonical source 已包含按卡 START admission fence/hold-release/fail-closed 逻辑。
它解决的是软件 admission race，不应被扩大解释为：

- 已证明历史现场 startup-loss 的唯一根因；
- 已排除 NIC/driver/socket/FPGA 的独立丢包来源；
- 已证明额外 startup trigger 的底层产生机制。

现场因果分析仍应使用 stage-1/stage-2/control trace 与必要的系统抓取。

## 4. Physical round / RoundIdentity

生产 round policy 由 shared `PhysicalRoundNormalizer` 负责。

```text
RoundIdentity = (measurementSession, roundGeneration)
```

主要规则：

- `startupFilterTriggerCount=X`：过滤每轮前 X 个 **new distinct physical trigger identities**；
- 多卡同一 physical trigger 共享一个分类结果，不按 packet/card 重复消费 slot；
- `disableCountBoundary=false`：configured logical count 保持 CountBoundary/final 语义；
- `disableCountBoundary=true`：configured count 只限制 realtime imaging；
  raw/save 在同一 physical round 中继续，直到 timeout；
- active physical-idle timeout 由 PAimage 20 ms poll chain 单一负责；
- timeout 可关闭 partial-startup round；
- timeout 后 AutoSave binding、old presentation capture、Ring residual discard/reset、
  stale cutoff 按冻结顺序执行；
- early/variable round 不生成 synthetic completion 或 synthetic partial Ring block。

详细历史证据：
`CODEX_REPORTS/session-abcd-closeout-20260918/`。

## 5. 保存与成像隔离

保存与实时成像是独立执行边界：

- CardFrame 进入有界 save queue / FileSaver；
- realtime Ring feed 经 `ImagingBypass` 独立有界队列；
- 成像服务未就绪、队列满、stale session 等不会被当作 UDP ingress loss；
- 成像旁路丢弃不应改变已接受 raw/save 对象的 round ownership；
- timeout 后旧 identity 的 sync/presentation fail closed，不重新进入新 round。

历史隔离实现与测试材料已归档到
`CODEX_REPORTS/acquisition-startup-history-20260907-13/imaging-isolation-20260911/`。

## 6. Trigger-gap / UI diagnostics

当前 production full-trigger gap owner：

```text
SourceCore per-card progression
 -> Decision::TriggerGap
 -> NetworkControllerPaimage observation sink
 -> CardStats
```

UI 语义：

```text
缺失   = triggersPartial
跳号数 = missingTriggerCount
丢包   = packetsDropped
已采集 = Normalizer physical distinct count
已过滤 = Normalizer startup-filtered count
```

same-session trigger sequence reset recovery 的 back-jump threshold 为 256；
small backstep 不计 gap、不 re-anchor。

## 7. Ring / CUDA production boundary

环形成像生产链使用：

```text
ImagingBypass
 -> RingBlockAssembler
 -> ImagingController
 -> ImagingSvc
 -> ring_recon_cuda
```

RoundIdentity 是 Ring / UI / service 边界的 ownership key。旧的按固定块数推断 round、
独立 UI timeout owner、synthetic completion 等方案均不是当前行为。

CUDA rebuild / dependency / delivery 规则不要在本 README 维护第二套版本；
统一遵循根目录 `BUILD_STANDARD.md`。

## 8. Build

正式 Windows 默认入口：

```powershell
Set-Location <repo>\MC_410T_MultiCard\delivery
cmd /c build_mingw_debug.cmd
```

正式构建必须：

- 在待交付 exact commit 上重新 configure；
- tracked tree clean；
- 使用 BUILD_STANDARD 规定的 Qt/MinGW/CMake/Ninja 与 runtime provenance；
- 记录 BuildIdentity 和关键 DLL SHA256。

主要产物：

```text
PAimageReceiverDiagnostics.exe
ImagingSvc.exe
ring_svc_selftest.exe
ring_udp_replay.exe
```

## 9. 当前文档入口

- `docs/README.md` — 当前模块文档索引；
- `docs/诊断日志导出.md` — 诊断 ZIP 的使用与判读；
- `docs/ZeroMQ依赖说明.md` — 当前 ZeroMQ runtime/import-library 约束；
- `tools/paimage-trace-schema.md` — PAimage trace schema；
- `tools/startup-looplog-schema.md` — startup loop log schema；
- `src/RingRecon/README.md` — CPU reference/verify 工具；
- `src/RingReconCuda/README.md` — CUDA source/rebuild/selftest 说明。

M2/M3、旧 UDP 模拟接入、旧 SHM/round 方案和阶段测试报告均已归档到
`CODEX_REPORTS/`，不再作为当前产品说明。
