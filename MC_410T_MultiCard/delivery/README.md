# PAimageReceiverDiagnostics — 当前生产采集与诊断说明

## 当前状态先读这里

本 README 描述 `main` 中当前正式生产架构。项目当前整体状态以仓库根目录 `PROJECT_STATUS.md` 为准，构建/交付以 `BUILD_STANDARD.md` 为准。

截至 2026-09-13：

- `main` 仍是正式源码基线；
- `main` **尚未包含** START admission fence 修复；
- 软件验收通过、等待真实 FPGA/NIC 实机测试的候选为：

```text
codex/start-admission-fence-fix-20260913-003112
6313540f72544c0f68820c4815903abaa0b8c1e1
```

该候选已完成独立软件审查并 `APPROVE`，但实机测试未完成，所以保持未合并 `main`。对启动段问题的现场结论必须区分：

```text
main 当前行为
vs.
START admission 候选 6313540f... 的行为
```

不得把候选分支的软件结果写成 `main` 已经修复，也不得把软件 `APPROVE` 写成真实 FPGA/NIC 丢包问题已经解决。

## 概述

`MC_410T_MultiCard/delivery` 是 Windows 多卡实时采集与成像主程序。当前 `main` 使用 **PAimage-derived acquisition backend**：WinSock UDP 数据由 `PaimageAcquisition::SocketReceiver` 接收，`SourceCore` 负责会话门控、按卡触发组装和多卡同步，随后由 `HostOutput` 接入显示、保存、发布和实时成像接口。

历史 `MultiPortReceiver -> DataProcessor -> PacketAssemblyBuffer` 链路仍在源码和部分测试中保留，但不是当前生产 UDP 采集入口。

## 当前生产数据流

```text
FPGA cards
  -> Windows UDP sockets
  -> PaimageAcquisition::SocketReceiver
       单接收线程；select() + ready socket drain-to-WSAEWOULDBLOCK
  -> PaimageAcquisition::SourceCore
       measurement admission / trigger assembly / per-card completion
  -> HostOutput / FrameConverter
       -> CardFrame -> 保存路径
       -> SyncFrame -> DataProcessor::deliverAssembled
            -> DisplayBuffer
            -> Ring realtime feed
            -> FramePublisher（可选）
```

控制流：

```text
MainWindow
  -> NetworkController
  -> PaimageAcquisition::Backend
  -> ControlState
  -> ControlSocket
  -> CONFIG / START / STOP UDP command
```

CONFIG 成功条件是当前事务收到全部目标卡的 **60 字节 CONFIG ACK**。18 字节 ready packet 只表示卡就绪，不构成配置确认。自动发现同样以受控 CONFIG-ACK 探测作为采集卡身份判据。

## 接收线程与 socket 模型

`SocketReceiver` 使用一个 Windows 高优先级接收线程同时服务 feedback socket 与所有数据 socket。

- 主循环约 1 ms `select()` timeout；
- ready socket 持续 drain 到 `WSAEWOULDBLOCK`；
- 每个采样 socket 请求 64 MiB `SO_RCVBUF`；
- 性能分析必须关注多卡同步 burst 的公平性、drain 时长、receiver loop gap，以及 NIC/driver/Windows 栈在应用 `recvfrom` 之前的丢弃可能。

64 MiB 缓冲不是“不丢包”的证明。

## 采样格式与压力模型

```text
sample interval      = 4 ns = 250 MSa/s
bits/channel         = 32
A+B sample pair      = 8 bytes
UDP payload          = 1440 bytes
protocol header      = 4 bytes: packetSeq + triggerSeq
samplesPerTrig       = acqTimeNs / 4
bytes/card/trigger   = samplesPerTrig * 8
packets/card/trigger = ceil(bytes/card/trigger / 1440)
```

典型值：

| 采集窗口 | 每卡每触发 payload | UDP 包/卡/触发 |
|---:|---:|---:|
| 20 us | 40,000 B | 28 |
| 30 us | 60,000 B | 42 |
| 50 us | 100,000 B | 70 |
| 200 us | 400,000 B | 278 |

200 us / 200 Hz 时 payload 约为：

- 单卡 80 MB/s；
- 4 卡 320 MB/s ≈ 2.56 Gbit/s；
- 8 卡 640 MB/s ≈ 5.12 Gbit/s；
- 16 卡 1.28 GB/s ≈ 10.24 Gbit/s。

以上未计 Ethernet/IP/UDP、IFG、driver、系统抓取、诊断、复制、显示、保存和成像开销。

## `main` 的 START 会话边界

当前 `main` 的 START 事务仍是：

```text
receiver.prepareStart(session)
output.beginSession(session)
ControlSocket 逐卡发送 START
receiver.completeStart(sendSuccess)
```

在 `main` 中，`SourceCore::prepareStart()` 关闭 admission，直到 `completeStart(true)` 才重新打开。因此早期卡在自己的 START 后立即回传的数据可能已经被 socket 收到，但会在 SourceCore 被判为 `Disabled`。这正是已验收候选分支针对的软件竞态之一。

### START admission 候选分支

`6313540f...` 引入按卡 START admission state、bounded hold/release 和 fail-closed 语义，并补齐 failure/overflow/no-callback/analyzer/race 回归。软件层已经通过独立验收。

但以下仍必须通过实机确认：

- FPGA 收到 START 后的真实首包时序；
- NIC/driver/Windows socket 在启动 burst 下是否独立丢包；
- 现场跳号是否只由 admission race 导致；
- 系统抓取与 stage 1/stage 2 的现场因果闭环；
- 高负载下 downstream sync queue / 保存 / 成像是否产生独立淘汰。

## SourceCore 组包注意事项

当前 SourceCore 使用某 trigger **第一个到达的 packetSeq** 作为 assembly base。更小 packetSeq 若稍后到达，可能因 16-bit 差值回绕被判为 `OffsetOutside`。

因此 START admission 修复不能自动证明 first-arrival assembly、乱序、NIC 丢包或下游问题全部消失。实机测试仍应保留 packet-level trace。

单卡 incomplete CardFrame 也可能进入保存路径；`FrameConverter` 保留 `isComplete`，但 `FileSaver` 当前不以它做 hard gate。文件长度正常不等于触发完整。

## 诊断体系

主要证据层：

- stage 1：`recvfrom` 成功后的 raw ingress；
- stage 2：SourceCore / admission decision；
- stage 6：CONFIG / START / STOP 控制发送结果；
- receiver timing / LoopLog；
- 可选 socket timestamp；
- Pktmon / WPR 系统抓取；
- `tools/startup_diagnostics_analyze.py` 统一分析。

分层判断：

```text
Pktmon 有包，stage 1 无包
  -> NIC/driver/Windows socket/receiver 前后继续定位

stage 1 有包，stage 2 gate/admission 丢弃
  -> 应用 START/session 边界

stage 1 完整 + SourceCore complete，但下游缺失
  -> OutputWorkers / sync queue / Display / Ring / save

Pktmon 也无包
  -> 继续向 wire-side / NIC / driver / FPGA 取证
```

## 构建

正式构建规则不再在本 README 维护第二套版本，统一使用根目录：

```text
BUILD_STANDARD.md
```

默认 Windows 主工程入口：

```powershell
Set-Location <repo>\MC_410T_MultiCard\delivery
.\build_mingw_debug.cmd
```

默认：

```text
configure preset = mingw-debug
build preset     = mingw-debug-build
output           = build/mingw_debug/bin
```

如果构建当前 START admission 实机候选，必须 checkout `6313540f...`，但仍读取最新 `origin/main:BUILD_STANDARD.md`。不要把 `main` 源码混进候选分支。

## 实机测试最低证据

对 START admission 候选，建议至少保留：

1. build manifest + exact SHA + dependency hashes；
2. 多轮 START/STOP 的 stage 1/stage 2/control trace；
3. 首触发/首包和各卡 START 时间关系；
4. partial/incomplete/cross-session/double-release/overflow 等计数；
5. 出现问题轮次的系统抓取（必要时 Pktmon/WPR）；
6. 下游 sync queue / save / ring 是否有独立丢弃证据。

测试完成前，正式结论只能是：

```text
START admission 软件竞态已修复并通过软件验收；真实 FPGA/NIC 现场问题仍待验证。
```
