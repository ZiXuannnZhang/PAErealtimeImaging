# PAErealtimeImaging

PAErealtimeImaging 是 Windows 平台实时光声成像项目工作区，包含多卡 UDP 采集、实时数据处理、环形扫描重建、CUDA 成像服务、保存、诊断、测试以及半径标定工具。

## 当前入口

开始任何开发、构建、验证或本地工作区整理前，按顺序读取：

1. `PROJECT_STATUS.md` — 当前项目状态、待验证分支、下一步工作。
2. `REPOSITORY_BASELINE.md` — canonical branch、分支角色与 merge/history 治理。
3. `BUILD_STANDARD.md` — 默认工具链、CMake preset、CUDA/Qt/ZeroMQ、产物与交付规范。
4. `Codex-GitHub双端联动快速上手.md` — ChatGPT / Codex Desktop 双端协作流程。
5. 当前 `codex/task-docs:TASKS/<task>.md` — 本次任务的精确目标、commit、测试和显式 override。

根目录 `HANDOFF.md` 已降级为历史兼容入口，不再承载当前项目状态。

## Canonical branch

`main` 是本仓库唯一 canonical branch，也是所有正式源码与仓库级正式文档的唯一基线。

- 新实现任务默认从最新 `origin/main` 创建独立实现分支。
- `codex/task-docs` 仅用于任务文档通信，不是实现基线。
- `backup/*`、旧诊断/实验分支、`master` 等只用于历史追溯。
- 不允许把历史分支隐式当成下一任务的开发基线。

## 当前 START admission 状态

当前 `main` **尚未包含** START admission fence 软件修复。

软件验收通过、等待真实 FPGA/NIC 实机测试的候选分支是：

```text
codex/start-admission-fence-fix-20260913-003112
6313540f72544c0f68820c4815903abaa0b8c1e1
```

该分支已经完成独立源码审查和确定性软件验收，结论为 **APPROVE**。但真实 FPGA/NIC 实机测试尚未完成，所以当前策略是：

```text
保持独立分支，不合并 main；先构建 6313540f... 并完成实机验证。
```

因此不能把“软件 admission race 已修复”表述成“现场丢包问题已经解决”。现场问题仍需通过真实 FPGA/NIC、应用 stage 1/stage 2/control trace，以及必要的 Pktmon/WPR 证据闭环。

## 当前生产采集链路（main）

正式运行入口使用 **PAimage-derived acquisition backend**。旧 `MultiPortReceiver` 仍保留用于兼容、测试和历史功能，但不是当前生产 UDP 采集入口。

```text
FPGA cards
  -> Windows UDP sockets
  -> PaimageAcquisition::SocketReceiver      单接收线程，多数据端口
  -> PaimageAcquisition::SourceCore          会话门控、按卡触发组装、多卡同步
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

CONFIG 以当前事务收到的 **60 字节 CONFIG ACK** 作为配置确认；18 字节 ready 只表示卡就绪，不等价于 CONFIG 成功。自动卡发现同样以受控 CONFIG-ACK 探测作为身份判据。

## 接收调度与负载模型

`SocketReceiver` 使用一个高优先级 Windows 接收线程同时服务 feedback socket 和所有数据 socket。主循环使用约 1 ms 的 `select()`；ready socket 会持续 drain 到 `WSAEWOULDBLOCK`。每个采样 socket 请求 64 MiB `SO_RCVBUF`。

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

当前 `main` 已具备 raw ingress、SourceCore decision、control trace、receiver timing/LoopLog 和系统抓取工具。定位启动缺失时应按证据层级判断：

```text
Pktmon 有目标包，stage 1 无包
  -> Windows 网络栈 / socket / receiver 之前或之间继续定位

stage 1 有包，stage 2 显示 admission/gate 丢弃
  -> 应用会话边界问题

stage 1 完整、SourceCore complete，但下游缺失
  -> OutputWorkers / Display / Ring / 保存等下游路径

Pktmon 也没有目标包
  -> 继续向 NIC / driver / 链路 / FPGA wire-side 取证
```

“首个可见 trigger”不等于“物理首 trigger”；16 位 triggerSeq 跨 measurement round 不能无约束关联。

## 构建与实机交付

正式构建规范见 `BUILD_STANDARD.md`。

当前默认 Windows 构建组合：Qt 6.8.0 + MinGW 13.1 + Ninja + CMake，主工程默认使用：

```text
configure preset : mingw-debug
build preset     : mingw-debug-build
script           : MC_410T_MultiCard/delivery/build_mingw_debug.cmd
```

注意：构建规范位于 `main`，但**实际构建对象必须是任务指定的实现分支/commit**。例如 START admission 实机候选必须构建 `6313540f...`，而不是因为规范在 `main` 就改为构建 `main`。

## 主要目录

- `MC_410T_MultiCard/delivery/`：当前 Windows 采集、处理、成像、诊断和测试主工程。
- `PALiveImagingSimSender/`：UDP 模拟发送器。
- `RadiusCalibration/`：半径标定工具。
- `CODEX_REPORTS/`：已执行任务的报告和证据索引。
- `_migration_pack/`：迁移期与预构建 CUDA fallback 资料；不是当前工作流入口。
- `docs/history/`：已降级的历史项目说明。

## 当前下一步

当前优先工作不是继续修改 START admission 软件逻辑，而是：

1. 按 `BUILD_STANDARD.md` 为 `6313540f...` 生成可追溯实机测试包；
2. 完成真实 FPGA/NIC 多轮 START/STOP 和目标负载测试；
3. 必要时同步采集 Pktmon/WPR 与应用 trace；
4. 实机验收通过后，再决定是否把 START admission 分支集成到 `main`。

完整状态始终以 `PROJECT_STATUS.md` 为准。
