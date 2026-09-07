# 项目交接说明（Handoff）

> 本文件夹是给下一个项目的交付包：只包含双波长分块实时重建（realtime_dual）的最终版脚本与必要函数，之前累积的过时版本（早期 loop/扇区/verify/CF 等）一律不包含。

## 1. 项目背景

项目目标是给环形扫描光声内窥成像做**接近实时的重建出图**：实际采集时，光声信号由 532nm/1064nm 双波长激发光**交替激发**（相邻 A-line 来自不同波长），数据预处理中已完成双波长通道切分（原始文件一列 = 一根 A-line，按奇偶列交替存放）。重建侧按采集系统**分块上传**的真实情况处理：每块收到一批双波长交替组合的 A-line，逐块预处理、增量反投影、实时刷新显示。

本交付包实现三种可选重建模式：波长一单独实时成像（`'wl1'`）、波长二单独实时成像（`'wl2'`）、双波长同时重建实时显示（`'both'`，左右双图同帧刷新）。

## 2. 文件清单（最终版）

| 文件 | 作用 |
| --- | --- |
| `Ringscan_DAS_loop_realtime_dual.m` | 分块实时重建主脚本（wl1/wl2/both 三种模式，含 VerifyFinal 整帧对比） |
| `simulateAcquisition.m` | 数据模拟发送模块：`'init'/'next'/'close'` 状态机，按块读取模拟真实分块到达 |
| `streamingReconAppend.m` | 真实采集逐块接入 API：原始块 → 逐块预处理 → 增量重建，一次调用 |
| `preprocessBlock.m` | 逐块预处理（列向步骤直接块级处理） |
| `das_recon_incremental_loop.m` | 增量反投影：每块 A-line 只反投影一次，状态跨调用保存 |
| `das_recon_circular_gpu_v2.m` | GPU 重建核心函数（延迟和累加 DAS，支持分层声速等） |
| `HANDOFF.md` | 本说明 |

## 3. 函数关联（分块数据接收实时重建链路）

```text
simulateAcquisition('next')  ──原始块──▶  streamingReconAppend(block, state, cfg)
（按块读文件/整帧切片）                  │
                                         ├─▶ preprocessBlock(block, pBlock)    逐块预处理
                                         └─▶ das_recon_incremental_loop(bscan) 本块全图反投影一次
                                                   └─▶ das_recon_circular_gpu_v2（GPU 核心，同目录）

Ringscan_DAS_loop_realtime_dual.m（主脚本）
  ├─ 取块：simulateAcquisition('init'/'next'/'close')
  ├─ 接入：streamingReconAppend（both 模式 wl1/wl2 各一次，状态独立）
  ├─ 显示：dispImg.wl1 / dispImg.wl2 双图同帧刷新
  └─ VerifyFinal：das_recon_circular_gpu_v2 全帧反投影作为整帧参考
```

主脚本只依赖本目录 4 个函数 + 核心函数，**自包含**，无其他项目内依赖。

## 4. 使用方法

```matlab
cd('实时重建脚本');
Ringscan_DAS_loop_realtime_dual;   % 默认 14.dat、both 模式
```

关键参数（主脚本参数段，均可改）：

- `ReconMode`：`'wl1'` / `'wl2'` / `'both'`（默认 both）。
- `AlinesPerFrame` / `AlinesPerBlock`：**双波长合计**口径（单波长数量 = 合计÷2，必须偶数）。14.dat 合计 8000（单波长 4000，40 块）；11.dat 合计 4000（单波长 2000，20 块）。每块原始列数 = `AlinesPerBlock`（一列一根 A-line）。
- `ShiftWL2`：1064 通道 circshift(+1) 列对齐（与全部主脚本约定一致，默认 1）。
- `CoverageDeg`：A-line 角度跨度（`dtheta = CoverageDeg/(AlinesPerFrame/2)`）。**部分覆盖时（如 11.dat 实际 180°）需同时把 `FOVDeg/FOVTheta0Deg` 设为覆盖扇区**，否则全圆显示会出现有限视角伪影；把覆盖角设成 360° 只是角度拉伸假象。
- `GridSize`：默认 0.02mm（1800²，单块约 1.3s）；0.05mm（720²）约 0.1~0.2s/块，仍远小于 5s 块间隔。

## 5. 测试数据

- 本地副本（当前工作区）：`DASredo/testdata/14.dat`、`DASredo/testdata/11.dat`（主脚本默认指向这里）。
- 原始路径：11.dat 在 `D:\zzx\data\20260716\`；14.dat 在 `Z:\zzx\20260519\`（Z 盘可能不可用）。

各数据文件参数：

| 数据 | wlOffset | sysDelay | Radius | AlinesPerFrame（合计） | 单波长 | 备注 |
| --- | --- | --- | --- | --- | --- | --- |
| 14.dat | 301 | [358, 371] | 6.48e-3（当前脚本为 6.57e-3，以实测为准） | 8000 | 4000 | SoundSpeedRadii=[]，SoundSpeeds=[1490 1540] |
| 11.dat | 151 | [171, 184] | 6.57e-3 | 4000 | 2000 | 实际覆盖角 180° |

## 6. 外部依赖与注意事项

- GPU：`das_recon_circular_gpu_v2.m` 需要 MATLAB GPU 支持（gpuArray）。
- `Freqfilter4FiberSignal`：`preprocessBlock` 在 `Gaussfil=1` 时调用，位于只读外部路径 `D:\zzx\data\SelfmadeFunction\`；默认 `Gaussfil=0`，无需该依赖。
- 本交付包主脚本已做自包含适配：`addpath` 只加本目录（核心函数同目录）；`folderPath` 默认指向 `../DASredo/testdata`。其余代码与正式版（`DASredo/dual/`）一致。

## 7. 验证结果（交付前实测）

- 14.dat、both 模式、40 块（0.05mm 网格）：逐块增量结果与整帧参考相对差异 ~7e-7（浮点求和顺序差异，数学等价）。
- 三种模式 wl1/wl2/both 与整帧参考一致；ShiftWL2 拼帧（incremental vs whole）精确为 0；单波长模式与 both 模式对应波长输出精确一致。
- 单块平均耗时（0.05mm）：14.dat 约 200ms（both）、11.dat 约 92~96ms；均远小于 5s 物理块间隔（200 A-line 合计 / 40Hz）。

## 8. 决策同步（2026-08-07，临时会话）

**ShiftWL2 首块不进行偏移。**

- 背景：1064 通道列对齐（circshift(+1)）在模拟实现中，首块用“帧末列”前插做 wrap；
  真实采集时首块到达时该列尚不存在，因此该 wrap 与实际采集不一致。
- 决策：**首块（block 1）的 1064 通道不进行偏移，直接使用本块自身数据**；
  从第 2 块起仍按原逻辑，用上一块的原始末列前插对齐。
- 影响：首块与整帧参考存在 1 列（约 0.09°）帧缝偏差，可接受。
- 待办（主会话）：按此决策调整 `simulateAcquisition.m` 与 C++
  `readFrameLastWL2`/`splitBlock` 的实现，去掉首块帧末列读取；HANDOFF 本文作为该决策的同步依据。