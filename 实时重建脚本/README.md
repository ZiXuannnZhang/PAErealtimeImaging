# 实时重建脚本 — MATLAB algorithm reference

本目录保存早期双波长环扫逐块重建 MATLAB reference。它对理解 preprocessing、
ShiftWL2、incremental DAS 和数值参考仍有价值，但**不是当前 production 配置或协议规范**。

production 实现位于：

```text
MC_410T_MultiCard/delivery/
  RingBlockAssembler
  ImagingController / ImagingSvc
  ring_recon_cuda
```

## 文件

- `Ringscan_DAS_loop_realtime_dual.m` — reference 主脚本；
- `simulateAcquisition.m` — 文件分块模拟输入；
- `streamingReconAppend.m` — 增量接入 reference；
- `preprocessBlock.m` — MATLAB preprocessing；
- `das_recon_incremental_loop.m` / `das_recon_circular_gpu_v2.m` — MATLAB DAS。

## 重要时效性说明

这些脚本保留历史默认值和历史数据路径，例如部分脚本仍含 200 MHz、DASredo/testdata、
11.dat/14.dat 的早期约定。当前真实采集为 250 MHz，不能直接把脚本默认值复制到 production。

另外，`simulateAcquisition.m` 当前仍保留“首块用帧末 wl2 列补 wrap”的模拟语义；
后续 C++ production 的首块/跨块处理已经演化。用 MATLAB 做 production 对照时，应先明确需要
比较的是历史 reference 还是当前 C++ semantics。

## 使用

可以在 MATLAB 中直接进入本目录运行主脚本，但必须先检查：

- 数据路径；
- DAQ/sample depth；
- radius / sound speed；
- coverage / angle；
- ShiftWL2；
- 当前要验证的 production 语义。

## 历史验证

早期脚本曾用于 M1 CPU reference 与后续 CUDA 数值对照。相关 M2/M3 阶段报告已归档到：

```text
CODEX_REPORTS/ring-reconstruction-history-202608/
```

原 `HANDOFF.md` 是当时的项目接力状态，其中含后来已变化的“待办/首块”描述，因此已由本 README
取代；原文仍可从 Git history 恢复。
