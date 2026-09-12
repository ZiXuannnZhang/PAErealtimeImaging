# PAimageReceiverDiagnostics — 当前生产采集与诊断说明

## 概述

`MC_410T_MultiCard/delivery` 是 PAErealtimeImaging 的 Windows 多卡实时采集与成像主程序。当前 `main` 的正式运行入口已经使用 **PAimage-derived acquisition backend**：WinSock UDP 数据由 `PaimageAcquisition::SocketReceiver` 接收，`SourceCore` 负责会话门控、按卡触发组装和多卡同步，随后由 `HostOutput` 接入既有显示、保存、发布和实时成像接口。

历史 `MultiPortReceiver -> DataProcessor -> PacketAssemblyBuffer` 链路仍在源码和部分测试中保留，但**不是当前生产 UDP 采集入口**。分析当前实机问题、性能和丢包时必须以 PAimage-derived 路径为准。

## 当前生产数据流

```text
采集卡 / FPGA
  │  UDP sampling data
  ▼
Windows UDP sockets
  │
  ▼
PaimageAcquisition::SocketReceiver
  │  单接收线程；select() + per-ready-socket drain-to-WSAEWOULDBLOCK
  │
  ▼
PaimageAcquisition::SourceCore
  │  measurement admission / trigger assembly / per-card completion
  │
  ├─ CardFrame ───────────────► HostOutput card worker ─► 保存路径
  │
  └─ same-trigger all-card sync
         │
         ▼
      SyncFrame
         │
         ▼
      HostOutput sync worker
         │
         ▼
      DataProcessor::deliverAssembled
         ├─ DisplayBuffer
         ├─ Ring realtime feed
         └─ FramePublisher（可选）
```

### 控制流

```text
MainWindow
  -> NetworkController
  -> PaimageAcquisition::Backend
  -> ControlState
  -> ControlSocket
  -> CONFIG / START / STOP UDP command
```

CONFIG 的成功条件是当前配置事务收到全部目标卡的 **60 字节 CONFIG ACK**。18 字节 ready packet 只表示卡就绪，不构成配置确认。

自动采集卡发现也以受控 CONFIG-ACK 探测为身份判据；ICMP/ARP 只用于辅助诊断，不用于决定“该 IP 是采集卡”。显式 `--target-ips` 模式可绕过自动发现并直接使用指定卡 IP。

## 接收线程与 socket 模型

当前 `SocketReceiver` 使用一个 Windows 接收线程同时服务：

- feedback socket（默认 UDP 8000）；
- 每卡一个数据 socket（默认从 UDP 8001 起连续分配）。

线程启动后请求 `THREAD_PRIORITY_HIGHEST`。每个采样 socket 请求 64 MiB `SO_RCVBUF`；feedback socket 同样配置大接收缓冲，因为它也可能承载采样数据。

主循环使用约 1 ms 的 `select()` timeout。某 socket ready 后，当前实现会持续 `recvfrom` / `WSARecvMsg` 直到 `WSAEWOULDBLOCK`，然后再处理下一个 ready socket。因此性能分析必须关注：

- 多卡同步启动 burst 时的 socket 服务公平性；
- 单次 drain 包数与 drain 时长；
- receiver loop gap；
- Windows/NIC/driver 层是否在应用成功 `recvfrom` 之前已经发生丢弃。

64 MiB `SO_RCVBUF` 是缓冲措施，不是“不丢包”的证明。

## 当前采样格式与压力模型

参数以 `include/AcqConfig.h` 和 `include/Constants.h` 为准。

### 数据格式

- 采样间隔：4 ns，即 **250 MSa/s**。
- 当前生产路径不做 README 旧版本所描述的 2:1 抽取。
- 每采样点包含 A、B 两通道。
- 默认 `bitsPerChannel = 32`。
- 因此 A+B 每 sample pair = `32 * 2 / 8 = 8 bytes`。
- UDP sampling payload = 1440 bytes。
- 每个数据报前 4 字节为采集协议头：16-bit `packetSeq` + 16-bit `triggerSeq`。

计算公式：

```text
samplesPerTrig = acqTimeNs / 4
bytesPerTriggerPerCard = samplesPerTrig * 8
packetsPerTrig = ceil(bytesPerTriggerPerCard / 1440)
```

### 典型采集窗口

| 采集窗口 | 每通道样本 | 每卡每触发 payload | UDP 包/卡/触发 |
|---:|---:|---:|---:|
| 20 us | 5,000 | 40,000 B | 28 |
| 30 us | 7,500 | 60,000 B | 42 |
| 50 us | 12,500 | 100,000 B | 70 |
| 200 us | 50,000 | 400,000 B | 278 |

### 200 us / 200 Hz 的 payload 量级

| 卡数 | payload 吞吐 |
|---:|---:|
| 1 | 80 MB/s ≈ 0.64 Gbit/s |
| 4 | 320 MB/s ≈ 2.56 Gbit/s |
| 8 | 640 MB/s ≈ 5.12 Gbit/s |
| 16 | 1.28 GB/s ≈ 10.24 Gbit/s |

该表仅统计采样 payload，不含 4 字节采集头、UDP/IP/Ethernet 开销、IFG、系统抓取、诊断日志、内存复制、显示、保存和成像成本。测试压力必须按**实际卡数 × 实际采集窗 × 实际触发频率**计算，不能再使用旧 README 中固定“约 98 KB / <=70 包/触发”的估算。

## START / STOP 会话边界

当前 `Backend::startMeasurement()` 的控制事务顺序是：

```text
receiver.prepareStart(session)
output.beginSession(session)
ControlSocket 逐卡发送 START
receiver.completeStart(sendSuccess)
```

`SourceCore::prepareStart()` 会关闭采样 admission 并清理上一会话残留；`completeStart(true)` 才重新打开 admission。因此 START 控制发送期间已经进入应用的采样包可以被 raw ingress trace 观察到，但 SourceCore 会把它们判为 `Disabled`。

由于 START 当前是逐卡 `sendto()`，较早收到 START 的采集卡理论上可能在其他卡 START 尚未发送完成前开始发送数据。这是当前“每轮启动段触发跳号和不完整触发”问题的**优先验证候选机制**，但在确定性测试和实机证据完成前不能直接宣布为唯一根因。

STOP 事务对应：

```text
receiver.prepareStop()
逐卡发送 STOP
receiver.completeStop(sendSuccess)
```

关闭 admission 后到达的尾包应只作为诊断证据存在，不应进入当前测量输出。

## SourceCore 组包语义

SourceCore 每卡独立维护当前 trigger assembly：

1. 新 trigger 的第一个可接受数据包建立 assembly。
2. **第一个到达的 packetSeq 被作为该 trigger 的 `basePacket`。**
3. 后续包槽位为 `uint16_t(packetSeq - basePacket)`。
4. 已见槽位判为 `Duplicate`。
5. 计算出的槽位超出当前触发期望包数判为 `OffsetOutside`。
6. 收齐期望唯一包数时输出 complete CardFrame。
7. trigger 变化或 100 ms 空闲超时时关闭当前 assembly；不足期望包数则输出 incomplete CardFrame。

因此，如果 packet 1 先于 packet 0 到达，packet 1 会成为 slot 0，随后到达的 packet 0 会因无符号差值回绕而成为 `OffsetOutside`。这一语义必须在启动 burst 的乱序/前导缺包分析中显式考虑。

多卡同步只接受 complete CardFrame；同一 trigger 的所有目标卡完整后才形成 SyncFrame。等待同步超过约 250 ms 或 pending 数量过多时会产生 `SyncExpired`。

## 保存与实时输出

PAimage-derived 路径将单卡 CardFrame 和多卡 SyncFrame 分开消费：

- CardFrame 主要进入保存 worker；
- SyncFrame 进入显示、Ring realtime feed 和 publisher。

`FrameConverter` 会把 `CardFrame::complete` 映射到 `TriggerGroup::isComplete`。当前 `FileSaver` 不以 `isComplete` 做 hard gate，因此 **incomplete CardFrame 可能被保存**。保存文件尺寸正常不等价于触发完整；缺失区域可能包含组包器生成的零值或受 basePacket 语义影响的位置偏移。

Sync output 使用独立队列。正常队列在积压超过约两个 block 时会淘汰最旧 block，因此“显示/成像触发跳号”必须与 raw ingress / SourceCore 层问题分开判断。

## 当前重点故障：启动段跳号与丢包

当前主要现场问题是每轮 START 后的启动段可能出现：

- 首部 triggerSeq 跳号；
- 某 trigger 收到的 packet 数不足；
- 多卡之间启动首包/首触发不一致。

当前源码审核已经确认以下风险需要优先验证：

1. START 发送窗口内 SourceCore admission 关闭，进入应用的采样包会被判 `Disabled`。
2. START 是逐卡串行发送，存在“前一张卡已经回数据、后一张卡 START 尚未发送”的可能。
3. 单接收线程对 ready socket 使用 drain-to-empty 策略，多卡同步 burst 时需验证公平性和 socket 积压。
4. SourceCore 以 first-arrival packetSeq 为 assembly base，前导乱序/缺包可能被转化或放大为 `OffsetOutside` / incomplete。
5. 当前自动化测试大多在 START transaction 完成后才开始注入采样数据，没有覆盖“卡收到自己的 START 后立即发首触发”的真实竞争。

这些是候选机制，不是未经实机证据的最终根因声明。

## 诊断体系

### Raw ingress trace

默认生产诊断启用 raw trace。`SocketReceiver` 在成功 `recvfrom`/`WSARecvMsg` 后、进入 feedback demux / admission / dedup / SourceCore **之前**写 stage 1 record，包含：

- monotonic timestamp；
- measurement session；
- ingress correlation id；
- source IPv4；
- local/source port；
- datagram length；
- triggerSeq；
- packetSeq。

因此 stage 1 是“应用已经成功从 socket 取得该数据报”的证据。

### SourceCore decision

stage 2 记录 SourceCore 对数据/assembly 的 decision，包括：

- `Disabled`
- `RecentTrigger`
- `Duplicate`
- `OffsetOutside`
- `Complete`
- `TriggerSwitch`
- `Timeout`
- startup / stop / listener cleanup decisions

### Control trace

控制发送记录为 stage 6，可用于关联逐卡 CONFIG / START / STOP 的发送时间、目标卡和发送结果。

### Timing / LoopLog

诊断等级 >= 1 时保留 select、drain、recvfrom、Core mutex、Core ingest/poll、线程生命周期等时序聚合和关键样本。LoopLog 使用固定容量队列和后台窗口 writer；采样间隔超过 2 秒后的首包产生 burst marker，并冻结 burst 前后最多 5 秒的接收循环窗口。

### System capture

`diagnostic-tools` 包含受控的 Pktmon/WPR 工具链：

- `start_system_capture_admin.ps1`
- `stop_system_capture_admin.ps1`
- `system_capture_common.ps1`
- `Open-AdminCapture.cmd`
- `receiver-scheduling.wprp`

应用只负责准备会话请求与显示状态，不自动提权。系统抓取必须按当前 session/token/run/listen/cardIPs/ports 精确关联。

### Unified analyzer

`tools/startup_diagnostics_analyze.py` 汇总应用 raw trace、LoopLog、系统 PcapNG、WPR/clock 信息和 round 存储证据。

当前分析器可以输出逐包 ingress、启动首包、四卡启动时间差、25 ms 节奏、系统层 packet presence 和证据矩阵，但尚需进一步增强 START stage 6 -> stage 1 -> stage 2 的逐卡因果关联。

## 分层判定原则

遇到启动缺失时按下面的证据顺序判断，不要仅依据 UI 累计计数：

```text
Pktmon 有目标包，stage 1 无包
  -> Windows 网络栈 / socket / receiver 之前或之间继续定位

stage 1 有包，stage 2 = Disabled
  -> 应用 START admission 边界丢弃

stage 1 完整，SourceCore 也 complete，但 Sync/Display/Ring 缺失
  -> OutputWorkers / downstream queue / consumer 路径

Pktmon 未观察到目标包
  -> 不足以单独断言 FPGA 没发送或 NIC 丢包；需要结合抓取有效性并继续向 wire-side / NIC / driver 取证
```

原始入口第一个可见 trigger 不等于物理首 trigger。16-bit triggerSeq 跨 measurement round 不能无约束关联。

## 状态统计说明

PAimage-derived 后端会把 raw ingress 和 SourceCore decision 映射到既有 CardStats 字段。旧字段名称兼容历史 UI，但解释时应以诊断 trace 为准：

- `packetsReceived` / `socketPacketsReceived`：应用 raw ingress 成功记录；
- `triggersComplete`：SourceCore complete card frame；
- `triggersPartial`：trigger switch / timeout 关闭的不完整 card frame；
- `packetsDropped`：对已形成 incomplete frame 的估算缺包数；
- `sessionBoundaryPacketsDiscarded`：当前 admission 关闭时被 SourceCore 判 `Disabled` 的包；
- `assemblyDuplicatePackets`：SourceCore `Duplicate`；
- `assemblyOffsetOutOfRangePackets`：SourceCore `OffsetOutside`；
- `staleTriggerPacketsDiscarded`：SourceCore `RecentTrigger`。

注意：**整触发在 admission 关闭期完全没有进入 assembly 时，单靠 `packetsDropped` 不一定能表达该物理触发全部缺失。** 启动问题必须结合 raw trace 和 session/control 时间线分析。

## 目录结构（当前关键部分）

```text
delivery/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── AcqConfig.h
│   ├── DataProcessor.h
│   └── PaimageAcquisition/
├── src/
│   ├── MainWindow.cpp
│   ├── NetworkController.cpp
│   ├── DataProcessor.cpp
│   ├── FileSaver.cpp
│   └── PaimageAcquisition/
│       ├── Backend.cpp
│       ├── NetworkControllerPaimage.cpp
│       ├── ControlState.cpp
│       ├── ControlSocket.cpp
│       ├── SocketReceiver.cpp
│       ├── SourceCore.cpp
│       ├── HostOutput.cpp
│       ├── OutputWorkers.cpp
│       ├── OutputQueues.cpp
│       ├── FrameConverter.cpp
│       ├── TraceWriter.cpp
│       ├── TimingWriter.cpp
│       └── LoopLog.cpp
├── tests/
│   ├── paimage_network_test.cpp
│   └── paimage_core/
└── tools/
    ├── paimage_trace_analyze.py
    ├── startup_diagnostics_analyze.py
    ├── start_system_capture_admin.ps1
    └── stop_system_capture_admin.ps1
```

## 构建环境

当前开发环境以 Windows 10/11 x64、Qt 6.8.0、MinGW 13.1.0 / Ninja / CMake 为主要本地验证组合。项目也保留 MSVC/clang-cl 构建路径。ZeroMQ 与成像运行时由仓库/交付环境提供。

常用 MinGW Debug 构建入口：

```powershell
Set-Location <repo>\MC_410T_MultiCard\delivery
.\build_mingw_debug.cmd
```

线性成像运行库默认从 `libs/imaging` 读取。若 `cufft64_12.dll` 位于外部、已核验的部署包，先设置运行库目录再构建：

```powershell
$env:PAIMAGE_IMAGING_RUNTIME_DIR = 'D:\artifacts\CardDiscoveryFix'
.\build_mingw_debug.cmd build
```

该目录必须同时包含 `pa_recon_core.dll` 和 `cufft64_12.dll`；大型 CUDA DLL 不需要提交到 Git。

正式任务的具体构建与测试命令以 `TASKS/` 中当次任务文档为准。不要因为组件重放通过就推断真实 NIC/FPGA 链路无丢包。

## 当前验证边界

已有组件与集成测试覆盖 SourceCore、ControlState、SocketReceiver、OutputWorkers、协议、保存、诊断导出和部分多轮测量流程；已有合成 Pktmon/WPR 验证证明诊断工具链可工作。

但以下结论仍不能由本地自动化直接推出：

- 真实 FPGA 的 START 后首包时序；
- 实机 4001 次物理触发守恒；
- Realtek/ConnectX/NIC/driver 在启动 burst 下无丢包；
- Windows interface / Pktmon / application ingress 三层在实际问题轮次完全一致；
- 当前 START admission race 是否就是现场问题的唯一根因。

这些必须通过专门的确定性 START-race 测试和真实系统抓取闭环验证。
