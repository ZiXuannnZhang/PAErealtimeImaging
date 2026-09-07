# PALiveImaging 项目交接导读

本目录是环形扫描光声实时成像项目的交接文档，供后续对该项目做优化接力时
快速理解算法来源、C++ 实现现状与待办事项。

- `HANDOFF.md`：MATLAB 算法交付说明（原始交接，含参数口径与验证结果）；
- 各 `.m` 文件：MATLAB 参考实现（算法权威来源，不再维护，仅作对照）；
- 本文件：C++ 交付包（`PALiveImaging/delivery`）视角的接力导读。

## 1. 项目目标

环形扫描光声内窥成像实时重建：采集卡组（4 卡 × 2 通道 = 8 通道）同步采集，
532nm/1064nm 双波长交替出光；监听程序按每通道 200 A-line 组包，
双波长切分与 1064 跨块对齐在预处理中完成，CUDA DAS 增量重建并实时显示
双波长图像。

## 2. 已完成工作（里程碑）

| 阶段 | 内容 | 验证 |
| --- | --- | --- |
| M1 | MATLAB 双波长环形实时算法移植为 C++（CPU 版） | 与 MATLAB 数值对照一致 |
| M2 | CUDA 重建内核（ring_recon_cuda） | 与 MATLAB 参考一致；可视化验收通过 |
| M3-A/C | 8 通道扇区模型、逐 A-line 角度 DAS、双波长对齐 | 8ch 与 1ch 整圈 wl1 corr=1.0 |
| M3-D | 环形控制台（前端参数 + 双波长显示） | 图像验收通过 |
| M3-B | RingBlockAssembler 真实采集组包 + 主窗口 UDP 馈送 | 40Hz 回放 5 块全出，corr≈1 |
| 交付 | PALiveImaging 全真采集交付包 + 包外模拟发送器 | 同机联调 3 次全通过 |

## 3. 架构与数据流

```text
采集卡组（8 通道，FPGA 完成电压→差分相位解调）
  → UDP：每卡 8001+卡号，4 字节头(packetSeq/triggerSeq)+B/A 交织载荷
  → MultiPortReceiver → DataProcessor（按触发组包，int16/int32→频率 kHz）
  → DisplayBuffer（全分辨率频率快照）
  → MainWindow::feedRingImagingPulse（5ms 轮询）
  → RingBlockAssembler（按 triggerSeq 对齐 8 通道，每通道 200 根组块）
  → ImagingController::submitRingBlock（共享内存 + ZMQ）
  → ImagingSvc（双波长切分 → preprocessBlock → wl2 跨块对齐 → CUDA DAS）
  → 双波长帧 → 环形控制台实时显示
```

## 4. 关键文件映射（MATLAB ↔ C++）

| MATLAB（handoff/） | C++（delivery/） | 说明 |
| --- | --- | --- |
| `Ringscan_DAS_loop_realtime_dual.m` | `src/RingConfigDialog.cpp` + `src/MainWindow.cpp` | 主流程/参数设定与弹窗显示 |
| `simulateAcquisition.m` | `PALiveImagingSimSender`（包外模拟器） | 数据发送模拟 |
| `streamingReconAppend.m` | `src/ImagingSvc/ImagingSvc.cpp` | 分块接入重建 |
| `preprocessBlock.m` | `src/RingRecon/ring_recon.cpp` `preprocessBlock` | 预处理（DBR/延时截断） |
| `das_recon_incremental_loop.m` | `RingBlockAssembler` + `ImagingSvc` 增量状态 | 增量反投影 |
| `das_recon_circular_gpu_v2.m` | `src/RingRecon/ring_recon_cuda.cu` | CUDA DAS 核心 |

## 5. 关键约定（后续优化必须遵守）

- 一次触发 = 8 通道同步 = 8 根 A-line；奇触发 = 532nm，偶触发 = 1064nm；
- 通道勾选决定参与重建的通道数 N，图像扇区按 360°/N 逆时针均分，
  首通道扇区起点默认 180°（9 点钟方向）；
- 组包按单通道 A-line 计数（默认 200 根/块），数据为双波长交替一维数组，
  未切分、未对齐，由 ImagingSvc 预处理阶段切分对齐；
- DAS 输入为每根 A-line 一维时域信号（频率数据），与线性实例一致；
- 解调（电压→差分相位）发生在采集卡 FPGA，监听程序只做单位换算；
- 1064 跨块对齐：首块不偏移，后续块用上一块末根前插。

## 6. 构建与运行

- 运行：`bin\MC410T_Receiver.exe`；同机联调加 `--target-ips 127.0.0.1`；
- 真机：开始监听（自动扫描）→ 环形控制台启动重建 → 开始测量；
- 源码构建：需 Qt 6.8 MinGW、CUDA 12.8；先编译 `src/RingRecon` CUDA 模块，
  再 `build_mingw_debug.cmd`；
- 同机联调：包外 `PALiveImagingSimSender` 一键发送（详见其 README）。

## 7. 已知限制与待办（优化接力点）

- **F64 模式**：每通道 8 光纤暂按单光纤处理，需扩展组包与预处理；
- **分层声速**：控制台分层边界数需为 0，DAS 分层路径尚未实现；
- **真机验证**：同机 UDP 已通，待真机全链路验证（触发时序、丢包行为、
  固定 IP 多卡）；
- **丢触发策略**：组包器对未完成触发最多缓冲 8 个后丢弃，真实采集
  高丢包率下的行为需实测调优；
- **性能**：当前单块约 75ms（4000×1600，360² 网格），如需更高速采集
  可优化 CUDA kernel（分块/共享内存/流水线）；
- **偶发启动**：子进程配置下发已加长到 1000ms；若后续出现偶发无帧，
  优先检查 ZMQ PAIR 连接时序与残留进程/共享内存；
- **前端参数**：采样深度、K、sysDelay 等需按真机标定值固化到默认配置。

## 8. 数据文件

- 14.dat（360°）：`D:\zzx\data\20260519\14.dat`，wlOffset=301，
  sysDelay=[358,371]，双波长合计 8000 A-line；
- 11.dat（180°）：`D:\zzx\data\20260716\11.dat`，wlOffset=151，
  sysDelay=[171,184]，双波长合计 4000 A-line。
