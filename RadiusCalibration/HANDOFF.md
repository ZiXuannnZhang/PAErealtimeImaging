# HANDOFF — RadiusCalibration 接力手册

> 面向：完全没有上下文的新会话。只描述 `RadiusCalibration` 文件夹内的工作，不涉及工作区其他代码。

## 0. 一句话说明

这是一套**独立于实时成像程序的 MATLAB 半径自校准脚本**。目标：在采集前对 8 个通道做全孔径扫描，估计每通道真实扫描半径（及相对起始角度偏移），把结果填入实时程序的“配准模式”。

## 1. 我们在做什么

实时程序的多通道合成孔径成像是“用数量换时间”：多个传感器各自短扫描合成大孔径。因装配公差，各通道实际扫描半径不同，配准模式允许每通道使用各自半径。本工作用离线 MATLAB 脚本完成：

1. 每通道做**全孔径扫描**，得到完整 360° 的 Bscan；
2. 对每通道做多半径重建；
3. 参考通道用**声聚焦效果**确定最佳绝对半径；
4. 其余通道的重建图与基准图做**归一化互相关**，消除通道间半径相对误差；
5. 各通道起始角度不同时，用 Bscan 列方向的**整数循环互相关**估计位移，通过角度向量虚拟对齐，不改原始数据。

## 2. 文件清单

| 文件 | 作用 |
| --- | --- |
| `radius_calibration.m` | 主脚本：读取 → 预处理 → 角度对齐 → 参考聚焦/其余互相关标定 → 验证/可视化/保存 |
| `read_ring_bscan.m` | 读取监听程序保存数据，返回每通道全孔径原始 Bscan（cell，不改数据） |
| `read_ring_saved_channel.m` | 读取单通道文件 `Card{N}_Ch{A|B}_{z}_{seq}.dat`，float16 转 single，按序号顺序拼接 |
| `half_to_single.m` | IEEE 754 half → single 逐位转换 |
| `align_channel_start_angles.m` | 整数列循环互相关，只输出位移列数 |
| `norm_corr2.m` | 零位移归一化相关系数 |
| `preprocess_ring_block.m` | Ringscan_DAS_loop_realtime_dual/preprocessBlock 的完整移植 |
| `recon_ring_das_multir.m` | 当前 CUDA `ring_das_kernel`/`append_angles_radii`（非拼接）的 MATLAB GPU 移植 |
| `ring_focus_metric.m` | 聚焦度量（归一化 Tenengrad 等） |
| `parabolic_peak.m` | 三点抛物线顶点拟合 |
| `README.md` | 使用说明 |

## 3. 已经完成的内容

### 3.1 数据读取
- 监听程序保存文件规则：`Card{N}_Ch{A|B}_{z}_{seq:03d}.dat`；
- 物理通道代号 = `(N-1)*2 + (A=0, B=1)`，即通道1=Card1_ChA，通道2=Card1_ChB，…，通道8=Card4_ChB；
- 文件内容：无文件头、float16、按触发顺序追加；`sampleCount=4000`，`triggersPerFile=1000`；
- 已实现任意 1..8 通道数组选择、z 字符串输入、文件数量输入；
- 已按用户要求：**读取函数只返回原始 Bscan，不做任何预处理、不修改数据、不拼接、不做 circshift**；
- legacy 14/11.dat 模式已按要求移除，`read_ring_calib_data.m` 已删除。

### 3.2 预处理
- 已把 `Ringscan_DAS_loop_realtime_dual` 的预处理完整移植到 `preprocess_ring_block.m`；
- **当前默认只做延时截断 `DelayCut=1`**，其余步骤保留开关但全部默认关闭（DBR/削顶/滤波/中值/光纤去除等）；
- 外部函数 `Freqfilter4FiberSignal` 只在 `Gaussfil=1` 时调用，默认路径：
  `D:\zzx\data\SelfmadeFunction\DeconvolutionFuncPack`。

### 3.3 角度对齐（当前重构核心）
- `align_channel_start_angles(refBscan, candBscan)`：
  - 列能量签名 → 一维 FFT 循环互相关；
  - **只返回整数位移 `shift`**，语义为 `circshift(candBscan, shift, 2)` 与 ref 相关最大；
  - 已用合成随机 Bscan 验证 0/3/57/199 列位移全部正确。
- 主脚本不修改原始数据，通过角度向量虚拟对齐：
  - `stepDeg = 360 / (fullAlinesPerChannel/2)`；
  - `offsetDeg = -angleShift(ch) * stepDeg`；
  - wl1 角度 = `angleStartDeg + offsetDeg + mod(idx,K)*stepDeg`；
  - wl2 角度 = `angleStartDeg + offsetDeg + mod(idx+1,K)*stepDeg`
    （因为 wl2 原始列比 wl1 提前一列，不使用 circshift）。

### 3.4 半径标定
- 参考通道（首个启用通道）：多半径全孔径重建 → 聚焦度量峰值 → 抛物线细扫；
- 其余通道：多半径全孔径重建 → 与基准图 `norm_corr2` → 相关峰值 → 抛物线细扫；
- 输出完整 8 通道半径（未选择通道保持标称值）、每通道整数列位移和角度偏移。

### 3.5 验证状态
- MATLAB R2023a 下 `checkcode` 对全部 `.m` 文件 OK；
- 真实保存数据读取回归：每通道 `wl1/wl2 = [4000 x 2000]`，全部有限；
- 3 通道缩减搜索冒烟流程完整跑通；
- 尚未做正式全量标定（见第 4 节）。

## 4. 当前卡在哪里 / 尚未完成

1. **缺少确认为“每通道全孔径扫描”的标定数据**。
   当前 `testdata\01` 的 4 文件×1000 触发=4000 触发/通道，冒烟时按
   `fullAlinesPerChannel=4000` 使用；但角度对齐后通道互相关很低
   （如通道3 corr≈0.005），角度位移也较异常（87 列、1739 列）。
   这强烈提示当前测试数据可能**不是真正的全孔径逐通道扫描数据**，或目标不适用。
   **正式标定前必须向用户确认数据采集方式。**

2. **默认全搜索尚未在合适数据上完整跑过**。
   默认 `searchMm=0.10 / coarseStep=0.01 / refine=true`，
   8 通道全孔径（每波长 2000 线）预计耗时较长，应先小规模运行再全量。

3. **尚未做已知真值的合成端到端验证**。
   需要构造已知通道半径和已知起始列位移的合成 Bscan，验证：
   - 角度位移符号/幅度恢复正确；
   - 参考通道绝对半径、其余通道相对半径恢复正确。

4. **可能的方向性扩展（未定，需用户确认）**：
   - 当前互相关是零位移 `norm_corr2`；若角度对齐残差大，后续可讨论
     小范围平移/旋转搜索或亚列角度细化；
   - 当前整数位移符合用户要求；若需要亚列精度，必须经用户确认后再做。

## 5. 下一步计划（建议顺序）

1. 与用户确认并取得**真正的每通道全孔径扫描标定数据**；
2. 先写/跑一个合成数据验证脚本：
   - 已知点目标、已知每通道半径偏差、已知起始列位移；
   - 验证 `align_channel_start_angles` 的符号与主脚本 `offsetDeg` 的符号；
   - 验证参考聚焦/其余互相关的半径恢复精度；
3. 用真实全孔径数据跑小规模（2~3 通道、粗扫）冒烟，检查相关曲线形状；
4. 确认曲线合理后，跑 8 通道默认全搜索 + 细扫；
5. 输出 `radiusPerChannel` 给用户，做实时程序端到端验证。

## 6. 绝对不要再踩的坑

1. **不要对原始数据做 circshift 或其他数值修改。**
   角度对齐只能输出位移，然后改重建角度向量；这是用户明确要求。

2. **不要给 wl2 做 circshift。**
   wl2 相对 wl1 提前一列，由 `mod(idx+1,K)*stepDeg` 处理。

3. **不要把当前 `testdata\01` 当成全孔径标定数据来下结论。**
   它可能只是正常多通道采集数据；先用数据特征/用户确认判断。

4. **不要恢复 legacy 14/11.dat 兼容。**
   用户已明确移除；相关文件已删除，不要再加回来。

5. **不要默认开启预处理的其他步骤。**
   只允许 `DelayCut=1`；DBR、削顶、滤波等保留开关但必须默认 0。

6. **不要回到旧的 45° 扇区掩码/逐扇区聚焦标定思路。**
   该思路已被否定并废弃；当前是每通道全孔径 + 参考聚焦/其余互相关。

7. **角度位移符号极易搞反。**
   `align_channel_start_angles` 的约定是 `circshift(cand,shift,2)` 匹配 ref；
   主脚本用 `offsetDeg = -shift*stepDeg`。任何改动后必须用合成已知位移验证。

8. **参考通道的绝对半径偏差会传递给所有通道。**
   参考通道数据要选信噪比好、聚焦曲线明确的通道；必要时再讨论全两两相关/最小二乘。

9. **外部函数目录。**
   `Gaussfil=1` 才需要 `Freqfilter4FiberSignal`；默认路径
   `D:\zzx\data\SelfmadeFunction\DeconvolutionFuncPack`，运行前确认函数存在。

10. **工作区其他源码有其他会话未提交改动（拼接边界过渡），本目录独立，不要混入或提交。**
    `RadiusCalibration/` 目前未跟踪，提交前需用户确认。

## 7. 运行方式

```matlab
cd('D:\DSHWorkspace\realtime_imaging_migration\RadiusCalibration');
radius_calibration
```

常用配置：

```matlab
cfg.channelSel = 1:8;
cfg.fileSuffix = 'test';
cfg.filesPerChannel = 4;
cfg.fullAlinesPerChannel = 4000;  % 每通道每圈总 A-line（含双波长）
cfg.useAllRounds = false;
cfg.fs = 250e6;
cfg.sysDelay = [358 371];
cfg.DelayCut = 1;                 % 预处理只保留这一项
cfg.searchMm = 0.10;
cfg.coarseStepMm = 0.01;
cfg.refine = true;
```

## 8. 关键公式与约定速查

- 每通道每波长 A-line 数：`K = fullAlinesPerChannel / 2`
- 角度步长：`stepDeg = 360 / K`
- 对齐位移：`angleShift(ch)` 列，`offsetDeg = -angleShift(ch)*stepDeg`
- wl1 角度：`angleStartDeg + offsetDeg + mod(idx,K)*stepDeg`
- wl2 角度：`angleStartDeg + offsetDeg + mod(idx+1,K)*stepDeg`
- 参考通道半径：`argmax focus(I_ref(r))`
- 其他通道半径：`argmax norm_corr2(I_ch(r), I_ref)`
- 输出：完整 8 通道 `radiusPerChannel`，未选通道=标称 6.57 mm
