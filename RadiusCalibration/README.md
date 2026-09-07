# 环形扫描多通道半径自校准（离线预校准）

本目录是一套**独立于实时成像程序**的 MATLAB 半径自校准脚本，用于在采集前估计
8 个通道传感器的真实扫描半径，并把结果填入实时程序的“配准模式”每通道重建半径。

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `radius_calibration.m` | 主脚本：全孔径读取 → 预处理 → 角度对齐 → 参考通道聚焦/其余通道互相关标定 → 验证/可视化/保存 |
| `read_ring_bscan.m` | 读取监听程序保存数据，返回每通道全孔径原始 Bscan（不改数据） |
| `read_ring_saved_channel.m` | 读取单通道保存文件（被 read_ring_bscan 调用） |
| `half_to_single.m` | float16 → single 逐位转换 |
| `align_channel_start_angles.m` | 整数列循环互相关，估计通道相对起始角度位移（只输出位移） |
| `norm_corr2.m` | 零位移归一化相关系数（通道重建图与基准图比较） |
| `preprocess_ring_block.m` | Ringscan_DAS_loop_realtime_dual/preprocessBlock 的完整预处理移植 |
| `recon_ring_das_multir.m` | 当前 CUDA `ring_das_kernel`/`append_angles_radii`（非拼接路径）的 MATLAB GPU 移植 |
| `ring_focus_metric.m` | 聚焦度量（Tenengrad / 归一化 Tenengrad / Brenner） |
| `parabolic_peak.m` | 三点抛物线顶点拟合 |

## 运行环境

- MATLAB + Parallel Computing Toolbox + 可用 CUDA GPU
- 本机已验证：MATLAB R2023a + NVIDIA GPU（sm_89）

## 监听程序保存数据格式

文件由 `FileSaver` 写出，规则见 `FileSaver::generateFileName`：

- 命名：`Card{N}_Ch{A|B}_{z}_{seq:03d}.dat`
  - `N`：采集卡编号（1..4），每卡两个通道 A/B；
  - 物理通道代号 = `(N-1)*2 + (A=0, B=1)`，对应 UI 的通道 1..8；
  - `z`：自定义字符串（保存时填写的后缀）；
  - `seq`：从 000 开始的组号，每满 1000 个触发递增。
- 内容：**无文件头、float16、按触发顺序追加**；
  每触发 `sampleCount` 点（当前 4000），文件字节数 =
  `2 × sampleCount × triggersPerFile`。
- 触发奇偶即波长：第 1、3、5… 列为 532nm，第 2、4、6… 列为 1064nm。

## 运行方法

```matlab
cd('D:\DSHWorkspace\realtime_imaging_migration\RadiusCalibration');
radius_calibration
```

主脚本顶部配置示例（按实际测试数据）：

```matlab
cfg.dataDir = fullfile(fileparts(scriptDir), 'testdata', '01');
cfg.channelSel = 1:8;              % 选择 1..8 任意数量通道
cfg.fileSuffix = 'test';           % 文件名中的 z 段
cfg.filesPerChannel = 4;           % 顺序读取 000..003 四个文件
cfg.fullAlinesPerChannel = 4000;   % 每通道每圈总 A-line 数（含双波长，全孔径标定）
cfg.triggersPerFile = 1000;
cfg.sampleCount = 4000;
cfg.useAllRounds = false;          % false=仅用第一圈标定；true=多圈全部参与
cfg.fs = 250e6;                    % 按实际采集采样率修改

% 预处理参数（默认只做延时截断，其余保留开关）
cfg.externalFunctionPath = 'D:\zzx\data\SelfmadeFunction\DeconvolutionFuncPack';
cfg.Gaussfil = 0;                  % 0=关闭光纤信号去除（1 时需外部函数）
cfg.filter_low = 0;                % 0=关闭高通滤波
cfg.filter_high = 0;               % 0=关闭低通滤波
cfg.med = 0;                       % 0=关闭中值滤波
cfg.DBR_sig_remove = 0;            % 0=关闭 DBR 置零
cfg.mask_length = 300;
cfg.singal_impair = 0;
cfg.im_value = [2000 400];
cfg.DelayCut = 1;                  % 只保留延时截断
cfg.sysDelay = [358 371];
```

## 读取与预处理

`read_ring_bscan(cfg)` 读取所选通道文件，返回：

```matlab
bscan.wl1{ch} : [sampleCount x Nwl] 通道 ch 的 532nm 原始 Bscan
bscan.wl2{ch} : [sampleCount x Nwl] 通道 ch 的 1064nm 原始 Bscan
```

- 每通道独立返回，不做拼接、不做 `circshift`、不做任何数值修改；
- 1064nm 相对 532nm 提前一列的角度差由重建角度向量处理。

主脚本在读取后调用 `preprocess_ring_block`，完整保留
`Ringscan_DAS_loop_realtime_dual` 的预处理步骤，但**默认只开启延时截断**
（`DelayCut=1`），其余开关默认 0。

## 标定原理（全孔径重构版）

1. 每个启用通道均做**全孔径扫描**，每通道每波长 `K = fullAlinesPerChannel/2`
   根 A-line，角度步长 `Δθ = 360°/K`；
2. 以首个启用通道为角度基准，其余通道用 Bscan 列能量签名做**整数循环互相关**
   （`align_channel_start_angles`）估计起始角位移；
3. **不修改原始数据**：把整数位移换算为角度偏移，在每通道角度向量中虚拟对齐；
4. 基准通道做多半径全孔径重建，用聚焦度量（默认归一化 Tenengrad）选最佳半径，
   得到基准重建图；
5. 其余通道做多半径全孔径重建，与基准图计算零位移归一化互相关
   （`norm_corr2`），相关峰值对应最佳半径，从而消除通道间半径相对误差；
6. 粗扫后抛物线拟合，再在峰值附近细扫（可关闭）。

## 主要配置

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `channelSel` | `1:8` | 启用的通道代号数组，1..8 任意数量 |
| `fileSuffix` | `'test'` | 文件名 z 段字符串 |
| `filesPerChannel` | `4` | 每通道顺序读取 000..N-1 |
| `fullAlinesPerChannel` | 4000 | 每通道每圈总 A-line 数（含双波长，全孔径标定） |
| `enableAngleAlign` | true | 是否做整数列循环互相关角度对齐 |
| `angleAlignWavelength` | 1 | 角度对齐使用 532nm Bscan |
| `searchMm` | 0.10 | 粗扫半范围（mm），覆盖装配公差上界 |
| `coarseStepMm` | 0.01 | 粗扫步长 |
| `refine` | true | 是否细扫 |
| `fineSpanMm` / `fineStepMm` | 0.012 / 0.002 | 细扫半范围 / 步长 |
| `calibWavelengths` | `1` | 标定用波长；`[1 2]` 双波长分别标定 |
| `useAllRounds` | false | true=多圈全部参与重建（时间×圈数） |
| `calibGridSize` | 0.1e-3 | 标定网格（米） |
| `validateGridSize` | 0.1e-3 | 验证重建网格 |
| `doFigures` | true | 预处理检查图 / 标定曲线图 / 参考通道前后对比图 |
| `saveResults` | true | 保存 `.mat` 与 `radiusPerChannel_mm.txt` |

## 输出与应用

脚本结束打印 8 通道完整半径（未选择通道保持标称值）与命令：

```text
--radius-per-ch "6.5700,6.5500,..."
```

- 先用 `ring_svc_selftest.exe --radius-per-ch ...` 做端到端数值验证；
- 实机使用时，在“环形扫描参数设定 → 配准模式”下把 8 个半径分别填入通道 1..8，
  或使用“设为默认”保存；
- 结果同时保存在 `RadiusCalibration/calib_out/`。

## 可视化检查

1. **预处理检查**：原始/预处理后的单通道 wl1/wl2 Bscan 预览；
2. **标定曲线**：参考通道为聚焦度量曲线，其余通道为与基准图的相关曲线；
3. **参考通道前后对比**：标称半径与标定半径的全孔径重建图。

## 注意事项

- 标定数据必须满足**每通道全孔径扫描**，且各通道观察同一静止目标；
- 角度对齐只输出**整数列位移**，不修改原始数据；1064nm 的提前一列同样由
  角度向量处理；
- 参考通道的聚焦最优半径是绝对锚点，其余通道只消除相对误差；
- `fullAlinesPerChannel`、`triggersPerFile`、`fs`、`sysDelay` 及预处理开关
  必须与采集时实际配置一致；
- `Gaussfil=1` 时需要 `Freqfilter4FiberSignal` 可用（`cfg.externalFunctionPath`
  指向其所在目录）；`filter_low/filter_high/med` 需要对应 MATLAB 工具箱。
