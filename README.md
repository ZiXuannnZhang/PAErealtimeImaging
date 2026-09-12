# PAErealtimeImaging

PAErealtimeImaging 是 Windows 平台实时光声成像项目工作区，包含多卡 UDP 采集、实时数据处理、环形扫描重建、CUDA 成像服务、保存、诊断、测试以及半径标定工具。

## Canonical branch

`main` 是本仓库唯一 canonical branch，也是所有正式源码与正式文档的唯一开发基线。

所有实现任务必须先同步最新 `origin/main`，再从 `main` 创建独立实现分支。`codex/task-docs` 只用于任务文档通信，不是实现基线；历史快照、诊断分支、迁移分支和实验分支也不得作为新的开发基线。

详细治理规则见：

- `REPOSITORY_BASELINE.md`
- `Codex-GitHub双端联动快速上手.md`

## 当前生产采集链路

截至当前 `main`，正式运行入口已经切换到 **PAimage-derived backend**。旧 `MultiPortReceiver -> DataProcessor -> PacketAssemblyBuffer` 代码仍保留用于兼容、测试和历史功能，但不是当前生产 UDP 采集入口。

当前实际链路：

```text
采集卡 / FPGA
  -> Windows UDP socket
  -> PaimageAcquisition::SocketReceiver      单接收线程，多数据端口
  -> PaimageAcquisition::SourceCore          会话门控、按卡触发组装、四卡同步
  -> PaimageAcquisition::HostOutput
       -> 单卡 CardFrame -> 保存路径
       -> 多卡 SyncFrame -> DataProcessor::deliverAssembled
            -> DisplayBuffer
            -> Ring realtime feed
            -> FramePublisher（可选）
```

控制链路与数据链路分离：

```text
UI / NetworkController
  -> PaimageAcquisition::Backend
  -> ControlState
  -> ControlSocket
  -> CONFIG / START / STOP UDP control command
```

CONFIG 仅以当前会话收到的 **60 字节 CONFIG ACK** 作为配置确认。18 字节 ready 包只表示卡片就绪，不等价于 CONFIG 成功。自动采集卡发现同样以受控 CONFIG-ACK 探测确认卡身份；ICMP/ARP 只作为诊断信息，不作为卡身份判据。

## 当前接收调度模型

`SocketReceiver` 当前使用一个高优先级 Windows 接收线程同时服务 feedback socket 与所有数据 socket。接收循环执行 `select()`，随后对 ready socket 逐个读取，单个 socket 会持续 drain 到 `WSAEWOULDBLOCK` 后才处理下一个 socket。

每个采样 socket 请求 64 MiB `SO_RCVBUF`。该缓冲可以吸收一定突发，但不能替代对多卡同步 burst、公平性和实际驱动/NIC 丢包的实机验证。

因此，评估接收能力时不要再使用旧 README 中“每线程 4 卡、多接收线程”的假设。

## 当前采样与 UDP 压力模型

当前生产数据格式以 `MC_410T_MultiCard/delivery/include/AcqConfig.h` 为准：

- ADC / FPGA 输出采样间隔：`4 ns`，即 **250 MSa/s**。
- 当前生产路径不再按 README 旧模型做 2:1 抽取。
- 每个采样点包含 A、B 两通道。
- 默认 `bitsPerChannel = 32`，因此每个 A+B sample pair 为 `8 bytes`。
- UDP 采样 payload 为 `1440 bytes`；每个 UDP 数据报另有 4 字节协议头：`packetSeq + triggerSeq`。
- 每触发样本数：`samplesPerTrig = acqTimeNs / 4`。
- 每触发期望 UDP 包数：`ceil(samplesPerTrig * 8 / 1440)`。

典型压力：

| 采集窗口 | 每通道样本数 | 每卡每触发 payload | 期望 UDP 包/卡/触发 |
|---:|---:|---:|---:|
| 20 us | 5,000 | 40,000 B | 28 |
| 50 us | 12,500 | 100,000 B | 70 |
| 200 us | 50,000 | 400,000 B | 278 |

以 200 us、200 Hz 为例，仅采样 payload 即约：

- 单卡：`400,000 * 200 = 80 MB/s`
- 四卡：`320 MB/s`，约 `2.56 Gbit/s` payload
- 八卡：`640 MB/s`，约 `5.12 Gbit/s` payload

以上未计 Ethernet/IP/UDP 头、IFG、驱动开销、诊断写入和用户态处理成本。做性能验证时必须按实际卡数、实际采集窗口和实际触发频率计算，不应使用旧的“约 98 KB / <=70 包”固定估算。

## 当前重点问题：每轮启动段触发跳号与不完整触发

当前主要实机问题是：**每轮采集开始阶段可能出现触发号跳变和 UDP 包缺失/不完整触发。**

现有源码审核已经确认下列事实，但这些事实仍需通过确定性测试和实机证据完成因果闭环：

1. `Backend::startMeasurement()` 的事务顺序为 `receiver.prepareStart()` -> 逐卡发送 START -> `receiver.completeStart()`。
2. `SourceCore::prepareStart()` 期间 admission 关闭；该窗口进入应用的采样 UDP 会记录为 raw ingress，但在 SourceCore 中被判定为 `Disabled`。
3. `ControlSocket` 当前按卡顺序逐个 `sendto()` START，因此较早收到 START 的卡有可能在其他卡 START 尚未发送完时已经开始回传采样数据。
4. 当前自动化测试主要在 START transaction 完成后才注入采样数据，没有覆盖“某张卡收到自己的 START 后立即回首触发”的硬件竞争。
5. `SourceCore` 当前以某 trigger **第一个到达的 packetSeq** 作为 assembly base；更小 packetSeq 若稍后到达会被判为 `OffsetOutside`。因此启动段前导丢包/乱序必须通过 raw ingress 证据与组包 decision 分开分析。

上述机制是当前优先验证对象，但在获得实机/系统层证据前，不能把它直接宣布为唯一根因。

## 诊断证据层级

当前 `main` 已具备以下诊断能力：

- stage 1：`recvfrom` 成功后的逐包 raw ingress trace，记录在 demux / admission / dedup / SourceCore 之前；
- stage 2：SourceCore decision，例如 `Disabled`、`Duplicate`、`OffsetOutside`、`Complete`、`TriggerSwitch`、`Timeout`；
- stage 6：CONFIG / START / STOP 控制命令逐卡发送记录；
- receiver timing / LoopLog：select、drain、recvfrom、控制 mutex、循环 gap、burst window；
- 可选 socket timestamp；
- 受控 Pktmon / WPR 系统抓取；
- `startup_diagnostics_analyze.py` 统一分析入口。

定位启动缺失时应按证据层级判断：

```text
Pktmon 有目标包，stage 1 无包
  -> Windows 网络栈 / socket / receiver 之前或之间继续定位

stage 1 有包，stage 2 = Disabled
  -> 应用 START admission 边界直接丢弃

stage 1 完整、SourceCore complete，但下游缺失
  -> OutputWorkers / Display / Ring / 保存等下游路径

Pktmon 也没有目标包
  -> 仅能说明系统抓取层未观察到该包；继续向 NIC / 驱动 / 链路 / FPGA wire-side 取证
```

## 数据质量注意事项

- SourceCore 的单卡 incomplete frame 可以进入保存路径；`FrameConverter` 会保留 `isComplete` 标记，但 `FileSaver` 当前不以该标记做 hard gate。因此分析原始保存文件时不能默认“文件长度正常 = 触发完整”。
- 多卡实时显示/成像走 SyncFrame 路径，要求同一 trigger 的所有卡先完成同步；下游 sync queue 发生积压时还可能产生独立于入口丢包的淘汰，需要用 trace stage 分层区分。
- “首个可见 trigger”不等于“物理首 trigger”。16 位 triggerSeq 跨轮次也不能直接关联，必须按 measurement session / round / time window 约束分析。

## 主要入口

- `HANDOFF.md`：项目整体状态与历史技术决策。
- `MC_410T_MultiCard/delivery/README.md`：当前采集、处理、诊断和构建细节。
- `MC_410T_MultiCard/delivery/src/PaimageAcquisition/`：当前生产采集后端实现。
- `MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py`：启动段统一诊断分析器。
- `MC_410T_MultiCard/delivery/tools/start_system_capture_admin.ps1` / `stop_system_capture_admin.ps1`：管理员系统抓取入口。
- `MC_410T_MultiCard/delivery/docs/`：阶段性设计与验收文档。
- `RadiusCalibration/HANDOFF.md`：多通道扫描半径标定工作。
- `REPOSITORY_BASELINE.md`：仓库基线与分支治理规范。

## ChatGPT <-> Codex Desktop 联动

- 任务文档专用分支：`codex/task-docs`
- 任务目录：`TASKS/`
- 任务文件命名：`<简要任务说明>_YYYYMMDD-HHMMSS.md`
- ChatGPT：架构、技术判断、任务规格、远端源码审查与最终验收。
- Codex Desktop：从最新 `main` 创建实现分支、修改源码、本地 Windows 构建/测试、提交并推送执行报告。
- Codex Desktop 的“测试通过”不等价于正式验收；ChatGPT 必须重新读取远端 HEAD、diff、源码和测试证据后给出 `APPROVE` 或 `REQUEST_CHANGES`。
