# PAErealtimeImaging

PAErealtimeImaging 是 Windows 平台实时光声成像项目工作区，包含多卡 UDP 采集、实时数据处理、环形扫描重建、CUDA 成像服务、保存、诊断、测试以及半径标定工具。

## 当前入口

开始任何开发、构建、验证或本地工作区整理前，按顺序读取：

1. `PROJECT_STATUS.md` — 当前项目状态、待验证分支、下一步工作。
2. `REPOSITORY_BASELINE.md` — canonical branch、分支角色与 merge/history 治理。
3. `BUILD_STANDARD.md` — 默认工具链、CMake preset、CUDA/Qt/ZeroMQ、产物与交付规范。
4. `Codex-GitHub双端联动快速上手.md` — ChatGPT / Codex Desktop 双端协作流程。
5. 当前 `codex/task-docs:TASKS/<task>.md` — 本次任务的精确目标、commit、测试和显式 override。

根目录 `HANDOFF.md` 仅作为旧工作流兼容入口；完整当前状态始终以 `PROJECT_STATUS.md` 为准。

## Canonical branch

`main` 是本仓库唯一 canonical branch，也是正式源码与仓库级正式文档的基线。

- 新实现任务默认从任务明确指定的 baseline 开始；没有 override 时才从最新 `origin/main` 创建。
- `codex/task-docs` 只用于任务规格与任务通道治理，不是实现基线。
- 已软件 APPROVE 的实现分支在硬件/system acceptance 未完成时可以继续保持未合并。
- `backup/*`、旧诊断/实验分支、`master` 等只用于历史追溯。

## 当前项目重点：物理轮次归一实机验收

当前最重要的待验收分支是：

```text
codex/physical-round-normalizer-integrated-20260916
52cf7713d7e0e935cb14663ec3470f3a25bfeb90
```

该分支的软件整改已经完成：RoundIdentity 数据面 barrier、ImagingSvc reconstruction identity barrier、SHM exact-seq 绑定、`sourceRoundComplete` / `reconstructionComplete` 分离、稳定 Final Marker 等均已通过自动化、Windows build 和真实 ImagingSvc/CUDA selftest。

但**物理轮次归一仍未完成硬件验收**。

新控制环境中观察到完整物理轮可能从旧环境约 `4001` 个 physical trigger 变为约 `4007` 个 physical trigger。因此当前不能继续把旧的：

```text
4001 physical -> 1 operational/control + 4000 logical
```

作为已证实现场协议，也不能机械改成：

```text
4007 physical -> 1 control + 4006 logical
```

当前第一优先级是本地解析最新大体量诊断日志，验证：

1. 排除有证据的手动暂停短轮后，完整轮是否严格稳定为 4007；
2. CountBoundary 是否真实发生在物理圈末、roundGeneration 是否正确推进；
3. TimeoutBoundary 是否形成 `timeout -> ring/reset -> ImagingSvc reset -> stale drop -> next clean round` 的完整证据链。

日志没有证据的部分必须明确标记“无法判断 / UNVERIFIED”，不能以软件自测结果代替实机证据。

当前状态：

```text
ROUND_IDENTITY_CODE_FIXES = IMPLEMENTED
ROUND_IDENTITY_AUTOMATED_TESTS = PASS
ROUND_IDENTITY_WINDOWS_BUILD = PASS
ROUND_IDENTITY_CUDA_SERVICE_SELFTEST = PASS
PHYSICAL_ROUND_NORMALIZATION_HARDWARE_ACCEPTANCE = PENDING
FULL_ROUND_TRIGGER_COUNT_4007 = TO_BE_VERIFIED_FROM_LOCAL_LOGS
COUNT_BOUNDARY_HARDWARE_BEHAVIOR = UNVERIFIED
TIMEOUT_BOUNDARY_HARDWARE_BEHAVIOR = UNVERIFIED
```

## START admission：独立待验收工作流

START admission 不是上述 4007 / PhysicalRoundNormalizer 问题的替代解释，应保持独立：

```text
codex/start-admission-fence-fix-20260913-003112
6313540f72544c0f68820c4815903abaa0b8c1e1
```

该分支的软件修复和确定性验证已经 **APPROVE**，但真实 FPGA/NIC 启动 ingress 行为仍未完成现场验收。因此仍不能把“软件 admission race 已修复”表述成“现场启动丢包已经解决”。

## 当前生产采集链路（main）

正式运行入口使用 **PAimage-derived acquisition backend**。旧 `MultiPortReceiver` 仍保留用于兼容、测试和历史功能，但不是当前生产 UDP 采集入口。

```text
FPGA cards
  -> Windows UDP sockets
  -> PaimageAcquisition::SocketReceiver
  -> PaimageAcquisition::SourceCore
  -> PaimageAcquisition::HostOutput / FrameConverter
       -> CardFrame -> 保存路径
       -> SyncFrame -> DataProcessor::deliverAssembled
            -> DisplayBuffer
            -> Ring realtime feed
            -> FramePublisher（可选）
```

控制链路：

```text
UI / NetworkController
  -> PaimageAcquisition::Backend
  -> ControlState
  -> ControlSocket
  -> CONFIG / START / STOP UDP control command
```

CONFIG 以当前事务收到的 **60 字节 CONFIG ACK** 作为配置确认；18 字节 ready 只表示卡就绪，不等价于 CONFIG 成功。

## 接收调度与负载模型

生产采样模型：

```text
sample interval       = 4 ns = 250 MSa/s
bits/channel          = 32
A+B sample pair       = 8 bytes
UDP sampling payload  = 1440 bytes
protocol header       = 4 bytes (packetSeq + triggerSeq)
samples/trigger       = acqTimeNs / 4
```

典型 payload：

| 采集窗口 | 每卡每触发 payload | UDP 包/卡/触发 |
|---:|---:|---:|
| 20 us | 40,000 B | 28 |
| 30 us | 60,000 B | 42 |
| 50 us | 100,000 B | 70 |
| 200 us | 400,000 B | 278 |

200 us / 200 Hz 时：单卡约 80 MB/s，4 卡 320 MB/s，8 卡 640 MB/s，16 卡 1.28 GB/s；未计 Ethernet/IP/UDP、IFG、driver、诊断和用户态处理开销。

## 诊断原则

定位问题时按证据层级判断，不跨层补结论：

```text
Pktmon 有目标包，stage 1 无包
  -> Windows 网络栈 / socket / receiver 路径继续定位

stage 1 有包，stage 2 显示 admission/gate 丢弃
  -> 应用会话边界问题

stage 1 完整、SourceCore complete，但下游缺失
  -> OutputWorkers / Display / Ring / 保存等下游路径

PhysicalRoundNormalizer 有 boundary 事件，但缺下游 reset 证据
  -> 只能证明 boundary 被记录，不能证明完整 reset 链成功
```

“首个可见 trigger”不等于“物理首 trigger”；`OperationalStartupControl` 是 host 侧 operational classification，不是已确认的 FPGA 协议 trigger type。

## 构建与实机交付

正式构建规范见 `BUILD_STANDARD.md`。默认 Windows 组合仍为 Qt 6.8.0 + MinGW 13.1 + Ninja + CMake：

```text
configure preset : mingw-debug
build preset     : mingw-debug-build
script           : MC_410T_MultiCard/delivery/build_mingw_debug.cmd
```

构建规范位于 `main`，但实际构建对象必须是任务指定的实现分支和精确 commit。不要为了读取最新规范而把 `main` 源码混入待验收候选。

## 主要目录

- `MC_410T_MultiCard/delivery/`：Windows 采集、处理、成像、诊断和测试主工程。
- `PALiveImagingSimSender/`：UDP 模拟发送器。
- `RadiusCalibration/`：半径标定工具。
- `CODEX_REPORTS/`：已执行任务的报告和证据；未合并候选的报告可能只存在于对应实现分支。
- `_migration_pack/`：迁移期与预构建 CUDA fallback 资料；不是当前工作流入口。
- `docs/history/`：降级的历史项目说明。

## 当前下一步

1. 本地解析最新实机诊断包，确认完整物理轮 trigger count 分布；
2. 以时间序列验证 CountBoundary 与真实圈末的对应关系；
3. 验证 TimeoutBoundary 的完整 reset/stale-drop/next-round 链；
4. 在事实明确前不修改 logical trigger count、first-visible filter、CountBoundary 语义或 FPGA/UDP；
5. 若日志证明当前归一模型不适配新控制环境，再创建独立源码整改任务；
6. START admission 继续作为独立硬件验证工作流推进；
7. 两条工作流分别达到硬件/system acceptance 后，再由用户决定是否以及如何集成回 `main`。

完整状态始终以 `PROJECT_STATUS.md` 为准。
