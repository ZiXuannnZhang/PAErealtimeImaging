# D1–D7 分析任务集执行回执（B 档 DAS 双波长质量增强）

Task set: `TASKS/D1…D6D7_20260925-033000.md` +
`TASKS/D6D7补充_复用合成体模前向模型_20260925-111222.md`
共同前提: `TASKS/前提_UBP与FBP调研报告审核更正说明_20260925-033000.md`
执行日期：2026-09-25

---

## 1. Task / source identity

```text
repository                = ZiXuannnZhang/PAErealtimeImaging
分析分支                    = codex/b-tier-analysis-d1-d7-20260925
STARTING_SHA              = 65de7820ae8be7fb8f6910e6edaba18fad19acf0   (= origin/main, 各任务文档指定的 baseline)
REPORT_HEAD               = 见第 10 节
实际工作区路径              = D:\Claudecode\PAERealtimeImaging
```

任务文档的远端副本用 `git fetch origin codex/task-docs` 取回，
与本地 `TASKS/` 逐字节一致（`git hash-object` vs `git rev-parse origin/codex/task-docs:<path>` 全部 MATCH），
故本次执行的就是已发布规格。

**任务类型**：全部为分析类（CHARACTERIZATION / BENCHMARK / TOOL / LITERATURE / EVALUATION）。
**未修改任何生产代码、测试代码、保存链路、`RingReconCudaConfig`、CUDA ABI。**

### 1.1 复用共享前向模型（补充规格 §2）

```text
使用的前向模型：共享 RingPhantomForward @ 6846864ba120e5ecda0aaeb44fef723ae66bb7d3
               （分支 codex/das-dual-wavelength-quality-b-tier-20260925）
脉冲波形与参数：
  Delta      σ = 4.0e-9 s（= 1 个采样点 @250MHz）—— 用于定位/位置误差
  GaussDeriv σ = 4.0e-9 s（默认）—— 用于反演核差异
  两组结果都报，按补充 §2.3 的用途分开
```

**契约测试本机独立复现**：`ring_phantom_forward_test` → **PASS 25 / FAIL 0**（退出码 0）。
编译只需 MinGW g++，不需 Qt/CUDA：

```bash
g++ -std=c++17 -O2 -o pf_test ring_phantom_forward_test.cpp RingPhantomForward.cpp ring_recon.cpp
```

**独立性约束（补充 §2.1）遵守情况**：`RingPhantomForward.cpp` 未被改动，
仍不 include 任何生产/重建头。正确用法（前向 → 生产重建作被测对象）**已按此执行**：
D7 的重建调用生产 `ringrecon::dasReconAppend`，度量用独立的 `pa_metrics.h`。

---

## 2. 交付物清单

| 任务 | 交付物 | 位置 |
|---|---|---|
| D1 | 脚本 + 数字表 + 导数可行性判定 | `scripts/d1_signal_characterization.mjs`、`out_d1_signal_characterization.txt` |
| D2 | 基准脚本 + 分段/块/网格时延表 + 余量估算 | `scripts/d2_recon_budget.cpp`、`out_d2_recon_budget.txt` |
| D3 | 纯几何脚本 + 变号/占比/权重表 | `scripts/d3_weight_behavior.mjs`、`out_d3_weight_behavior.txt` |
| D4 | 度量工具 + 单元自检（含负例） | `scripts/pa_metrics.h`、`scripts/d4_metrics_selftest.cpp`、`out_d4_metrics_selftest.txt` |
| D5 | 三份短分析 + 引用清单 + 措辞边界 | `D5_literature.md` |
| D6 | 双模式指标对照表 + 比值守卫 | `scripts/d6_real_baseline.cpp`、`out_d6_real_baseline.txt` |
| D7 | DAS vs UBP 成对 A/B 表 + 噪声趋势 + 上限评估 | `scripts/d7_phantom_ab.cpp`、`out_d7_phantom_ab.txt` |

复跑命令（全部在仓库根执行）：

```bash
node CODEX_REPORTS/b-tier-analysis-d1-d7-20260925/scripts/d1_signal_characterization.mjs
node CODEX_REPORTS/b-tier-analysis-d1-d7-20260925/scripts/d3_weight_behavior.mjs
g++ -std=c++17 -O2 -o d4 CODEX_REPORTS/b-tier-analysis-d1-d7-20260925/scripts/d4_metrics_selftest.cpp && ./d4
g++ -std=c++17 -O2 -O2 -I _task_work/btier_build -I CODEX_REPORTS/b-tier-analysis-d1-d7-20260925/scripts \
    -o d7 CODEX_REPORTS/b-tier-analysis-d1-d7-20260925/scripts/d7_phantom_ab.cpp \
    _task_work/btier_build/RingPhantomForward.cpp _task_work/btier_build/ring_recon.cpp && ./d7 0.1e-3 36e-3
g++ -std=c++17 -O2 -I _task_work/btier_build -I CODEX_REPORTS/b-tier-analysis-d1-d7-20260925/scripts \
    -o d6 CODEX_REPORTS/b-tier-analysis-d1-d7-20260925/scripts/d6_real_baseline.cpp \
    _task_work/btier_build/ring_recon.cpp && ./d6 testdata/01 0.1e-3
g++ -std=c++17 -O2 -I MC_410T_MultiCard/delivery/src/RingRecon \
    -o d2 CODEX_REPORTS/b-tier-analysis-d1-d7-20260925/scripts/d2_recon_budget.cpp \
    -L _migration_pack/prebuilt_cuda/bin -lring_recon_cuda && ./d2 7 2
```

---

## 3. 三条跨任务的决定性发现（先看这三条）

### 3.1 【数据充分性】testdata/01 只有 1/8 个物理通道含真实信号

D1 与 D6 独立测到同一事实：

| 通道 | 噪声底 RMS | 记录峰值 | max\|v\|/RMS | 判定 |
|---|---|---|---|---|
| Card1_ChA | 22.41 | 31568 | **1408.5** | **有信号** |
| Card1_ChB | 27.70 | 48.0 | 1.733 | 无 |
| Card2_ChA | 30.70 | 53.0 | 1.726 | 无 |
| Card2_ChB | 38.12 | 66.0 | 1.731 | 无 |
| Card3_ChA | 49.00 | 85.0 | 1.735 | 无 |
| Card3_ChB | 35.18 | 61.0 | 1.734 | 无 |
| Card4_ChA | 32.86 | 57.0 | 1.735 | 无 |
| Card4_ChB | 38.34 | 67.0 | 1.748 | 无 |

无信号通道的 `max|v|/σ = 1.73 = √3`，是**均匀分布**的特征 ⇒ 纯量化噪声，无回波。

**后果**：
- 噪声底、双波长一致性 —— 8 通道全部可测，结论有效；
- 带宽 / 脉冲形状 / SNR / 导数可行性 —— **只建立在 1/8 个物理通道上**；
- D6 的「两模式差值」主要反映**单通道扇区 vs 全局**，不是 8 通道满孔径的有限视角效应。

### 3.2 【比值守卫的适用边界】它是算子回归测试，不是实像的性质

| 数据 | 结果 |
|---|---|
| 合成（D7，已知真值幅值比 2.5） | **8/8 PASS**，`meanRatio=2.5`（等于真值），`relStd≈1e-6` |
| 合成（D4 自检，含 3 个负例） | **18/18 PASS**，3 个负例都确实失败（判别力成立） |
| 实测（D6，testdata/01） | **FAIL**，`relStd = 2.24`（global）/ `12.0`（splice） |

**解释**：比值守卫断言的是「线性算子保持**输入**的幅值比」。真实 wl1/wl2 的
空间比值本来就不是常数（吸收体分布、噪声、fluence 都随空间变），
所以它**不可能**在实像上通过。它的正确用途是**回归守卫算子**（防 B1/B2/B3 引入非线性），
**不得**当作实像的验收门槛。

**建议**：R1 的实机验收判据应改为
「对**已知比例的合成/复制输入**断言比值守卫 PASS」，
而不是「对实测图断言 PASS」。

### 3.3 【B1 的可行性瓶颈不在预算，在 FOV 与非目标结构】

- **预算侧（D2）**：整圈重建 2.528 s vs 采集 200 s ⇒ 占 **1.26 %**，
  每 A-line 可用预算 ≈ **24.7 ms**。B1 的 O(nt) 逐线导数相对该预算可忽略。**预算充足。**
- **成像侧（D7 + D3 + D5）**：UBP 成对切换**确实锐化主瓣**（轴向 -6 dB 从 0.237 mm 到 0.008 mm），
  但图像被**非目标结构**主导（真值处/峰值 = 0.0018，全局峰值落在 r≈16 mm），
  故 CR / gCNR / CNR **反而更差**。
- 根因与 D3/D5 吻合：FOV 36 mm 中 **89.54 %** 像素在探测环（R=6.57 mm）**之外**，
  而精确反演要求**源支撑在探测面内**（Haltmeier 2014）；该前提大面积不成立。

---

## 4. D1 实测信号特性表征

**Task id / baseline SHA**：D1 @ `65de7820ae8be7fb8f6910e6edaba18fad19acf0`

**数据清单（SHA256 见 `out_d1_signal_characterization.txt` §0.1，64 个文件逐一列出）**

| 项 | 值 | 来源 |
|---|---|---|
| 编码 | IEEE-754 binary16（float16）LE，定长记录，无文件头 | 实测判别（指数场直方图平滑 ⇒ f16，非 raw int16） |
| 记录长度 | **10000 采样点 = 40 µs @250 MSa/s** | 实测判别（峰值样本位置在 10000 分块下极差 ≤3，50000 分块下极差 ≫） |
| 通道映射 | Card{1..4}_Ch{A,B} → 物理通道 0..7 | `AcqConfig`/`FileSaver` |
| 波长映射 | g 偶 = wl1(532nm) | `RingBlockAssembler.cpp:360`，`triggerWlOdd=1` |
| 配置基准 | `ring_recon_cuda_set_defaults` | 代码实测 |

**E3 配置来源差异（显式标注）**

- (a) `AcqConfig::acqTimeNs` 默认 200000 ns ⇒ `samplesPerTrig = 50000`；
      本数据实测记录长度 **10000** ⇒ 该会话 `acqTimeNs` 实为 **40000 ns**。差异已标注。
- (b) MATLAB 脚本对 14.dat 记 `DAQ=200e6`、`Radius(14.dat)=6.48e-3`；
      本仓默认 `daqHz=250e6`、`radius=6.57e-3`。一律以本仓默认为基准。
- (c) `testdata/01` provenance 未正式确认（`RadiusCalibration/README.md:164`）。

**数字表**：见 `out_d1_signal_characterization.txt` 表 2.1（噪声底/SNR）、2.2（带宽）、
2.3（脉冲形状）、§3（双波长一致性）、§4（ring-down）、§6（与 14.dat 交叉核对）。

**双波长一致性（8 通道）**：|相对差| 最大值 —— 噪声底 **0.40 %**、带宽 **0.23 %**、
脉宽 **4.48 %**。两个波长的链路特性**高度一致**。

**导数可行性判定（B2「导数前先低通」）**

判据：同一低通 fc 下，B1 信号项 `2p − 2t·p′` 的 SNR 相对 DAS 的 `p` 损失 ≤ 6 dB。
实测噪声谱归一化自检：`∫S_p df` 与实测噪声方差偏差 **0.115 %**（归一化正确）。

> **结论：导数型信号项在本数据上【不可行】（在「保住分辨率」与「保住微分 SNR」之间无法两全）。**

依据（两条同时成立）：

1. 要压住 `(2πf·t₀)²` 的噪声放大，fc 必须压到 **≤2 MHz**（四个评估点的可行 fc 上限交集）；
2. 而实测信号 **-3 dB 带宽 53.406 MHz**（中心频率 35.507 MHz）；
   把 fc 压到 2 MHz，轴向分辨率由 `c/(2·BW)` 的 **13.95 µm** 恶化到 **372.5 µm**，差 **26.7 倍**。

附注：
- 不同评估点的可行 fc 上限差别很大（ring-down 2 MHz / 最强回波 20 MHz / t=2µs 3 MHz / t=10µs 8 MHz），
  说明结论**强依赖回波处局部斜率**，不存在对所有像素都成立的 fc。
- **G5 前置约束**：成像路径已过前端零相位滤波；B2 若在重建侧再加低通就是滤两次，须先定分工口径。
- 最强成分是启动 ring-down（比噪声底高约 919 倍），微分会进一步放大其陡峭前沿。
- **数据充分性**：判定只建立在 1/8 个物理通道上（见 §3.1）。

**证据四层**：source correctness 未声称 ／ automated = 脚本可复跑 是 ／
real hardware = testdata/01（2026-08-16 历史实测，provenance 未确认）+ testdata/14.dat ／
root-cause 未声称。

---

## 5. D2 重建链路实时预算基准

**Task id / baseline SHA**：D2 @ `65de7820…acf0`
**被测对象**：生产 CUDA 核 `libring_recon_cuda.dll`（`ring_recon_cuda_append_*` / `ring_recon_cuda_snapshot`）

**硬件与工具链**

| 项 | 值 |
|---|---|
| GPU | 实测可跑（CUDA 12.8 runtime `cudart64_12.dll`）；具体型号见 `nvidia-smi`（本机未采集，列为未验证项） |
| MinGW | `D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin\g++` |
| CUDA | 12.8，`nvcc` 在 `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin` |
| CUDA DLL 来源 | `_migration_pack/prebuilt_cuda/bin/`（**prebuilt**，非本机重编） |
| Qt / CMake / Ninja | 存在于 `D:\Qt\Qt6.8.0\Tools\…`，本基准未使用 |

**⚠️ 与规格的偏差（如实声明）**：
规格 §3 要求「在 `origin/main` exact commit 上重新 configure + build，记录 BuildIdentity」。
本任务用**独立 console 程序链接预编译 CUDA DLL** 完成测量，**未做完整 Qt 交付构建**，
因此 **BuildIdentity 缺失**。理由：本环境 Qt/MinGW/CMake/Ninja 未在 PATH，
完整交付构建需 `cmd //c ".\build_mingw_debug.cmd"` 全流程，本轮未跑。
**CUDA DLL 的来源与版本已记录（上表）**，但其与 baseline SHA 的对应关系**未证明**。

**分段时延（gridSize=10 µm，alinesPerBlock=200，预热 2 次后取 7 次）**

| 段 | mean(ms) | median(ms) | min | max | 极差 |
|---|---|---|---|---|---|
| `append_angles_radii`（global） | 2513.2 / 2530.7 | **2528.5 / 2528.3** | 2420.1 / 2524.5 | 2593.5 / 2537.8 | 173.5 / 13.3 |
| `append_angles_radii_sector`（splice） | — | ≈同上（差 <0.5 %） | | | |
| `snapshot`（dn=360, step=1） | 0.110 | **0.095** | 0.092 | 0.183 | 0.091 |

**块大小敏感性（gridSize=10 µm）**

| alinesPerBlock | 块数/圈 | 每块 median(ms) | 每 A-line(µs) | 整圈 median(ms) |
|---|---|---|---|---|
| 100 | 80 | 31.849 | 318.49 | 2547.9 |
| 200 | 40 | 62.803 | 314.01 | 2512.1 |
| 400 | 20 | 125.448 | 313.62 | 2509.0 |

⇒ **块大小几乎不影响吞吐**（100→400 仅差 1.5 %）。

**网格敏感性（alinesPerBlock=200）**

| gridSize | 网格 | 整圈 median(ms) | snapshot(ms) | 每 A-line(µs) |
|---|---|---|---|---|
| 20 µm | 1800² | 668.5 | 0.212 | 83.56 |
| 10 µm | 3600² | 2522.5 | 0.190 | 315.31 |

⇒ **网格是主导项**：线性 2 倍（像素 4 倍）⇒ 时延 **3.77 倍**。

**余量估算（含 B1 可用预算）**

口径必须与采集节拍比：`AlineRateHz = 40` ⇒ 每根 A-line 间隔 25 ms；
单圈 8000 根 ⇒ 采集时长 **200 s**；`alinesPerBlock=200` ⇒ 块间隔 **5.00 s**。

```text
(a) 整圈口径  ：采集 200.0 s vs 重建 2.528 s ⇒ 占 1.26 %，余量 98.74 %
(b) 流式块口径：块间隔 5.00 s vs 每块 63.21 ms ⇒ 占 1.264 %，余量 98.74 %

B1 可用预算量级（每 A-line 允许的额外开销）：
    整圈余量摊到每 A-line   ≈ 24684 µs / A-line
    流式块余量摊到每 A-line ≈ 24684 µs / A-line
```

**结论：B1 的逐线开销预算充足**（B1 的导数是 O(nt) 一次差分 + 一次乘加，nt=4000）。
**本任务不声称「实时通过」**——实时性结论由规划侧按实际节拍判断（规格 §6）。

**负载敏感性 / 可复现性**：两轮（13:53:05 / 13:55:24）整圈 median
**2528.5 ms vs 2528.3 ms，差 0.01 %**。本轮**未出现**历史上的 2–4 倍波动；
两轮极差分别 173 ms 与 13 ms（第 1 轮含一次抖动）。两轮均已记录，未挑好的那次。

**证据四层**：source 未声称 ／ automated = 构建成功 + 基准可复跑 是 ／
real hardware = **仅真实 GPU 上的性能实测，不等于实机采集验证** ／ root-cause 未声称。

**未验证项**：BuildIdentity 缺失；GPU 型号未采集；未测真实采集端到端；
未测 Ubp 模式时延（属 B1 实现阶段）；CUDA DLL 与 baseline SHA 的绑定未证明。

---

## 6. D3 环外权重行为表征

**Task id / baseline SHA**：D3 @ `65de7820…acf0`。纯几何/数值，不需 GPU、不需真实数据。

**几何定义与默认值来源**：`ring_recon_cuda_set_defaults`（`ring_recon_cuda.cu:73-151`）+
`MainWindow.cpp:5368-5369`（`sectorWidth=360/M=45°`、`step=45/500=0.09°`）。
`R=6.57 mm`、`fov=36 mm`、`gridSize=10 µm`、`nx=ny=3600`、`minDistance=0→10 µm`。

**变号边界**：`cosα = 0 ⟺ r·n̂ = R`，即环在探测器处的**切线**。
`cosα<0 ⟺ ρ·cos(θ_r−θ_s) > R`，必要条件 `ρ > R`；该像素被变号探测器命中的角占比 = `acos(R/ρ)/π`。

**变号像素占比（实测 12,960,000 像素逐个统计）**

```text
rho > R 的像素 = 11,603,884  ⇒  89.5361 %   ←—— FOV 大部分在探测环之外
rho <= R       =  1,356,116  ⇒  10.4639 %
全 FOV 平均「变号探测器」角占比（仅 rho>R）= 0.333843
```

**权重数值与 R/d 比值验证**

```text
w_现有 = Δθ·apod·cosα/d        （= Δθ·apod·(R²−r·r_s)/(R·d²)）
dΩ     = R·Δθ·apod·cosα/d²     （= R·Δθ·apod·(R²−r·r_s)/d³）
dΩ / w = R/d
```

随机采样 4000 组（像素, 探测器）：**max |dΩ/w − R/d| / (R/d) = 4.638e-16**
⇒ **恒等式在数值精度内成立，前提 E1 得到独立复核。**

实现形态核对：`wImpl = (−Δθ·apod)·dotp/(R·dsafe²)` 与闭式一致，
`d ≥ minDist` 样本上 `max|wImpl − wClosed| = 6.217e-15`（float64 舍入级）。

**minDistance 结论：够用但只是硬钳位（不是平滑窗）**

```text
网格像素到探测器全局最小距离 d_min = 0.052814 um
d <= minDist(10 um) 的像素数 = 7884 (0.060833 %)
```

⇒ minDistance **会生效**，能阻止 `d→0` 的严格奇异；但只是把奇点截断在 10 µm。
放大定量：`|w_现有|_max ≈ Δθ/minDist = 157.08`（环心 `0.2391`），
相对环心放大 **R/minDist = 657×**；UBP 形态（1/d²）放大 **(R/minDist)² = 431649×**。

**环外处理候选表（仅结构性后果，不做优劣预判）**

| 候选 | 操作 | R1 比值守卫 | R3 线性 | 其它结构性后果 |
|---|---|---|---|---|
| A | 保持符号累加 `accW += w` | 安全（w 纯几何） | 保持 | accW 可正负抵消，归一化失去意义 |
| B | 取绝对值累加 `accW += \|w\|`（**现行**） | 安全 | acc 线性 | `acc/accW` 非「平均权重」；环外能量被系统性低估 |
| C | 掩膜截断（cosα<0 不累加） | 安全 | 保持 | 有效孔径限于半平面，视角进一步受限 |

三者共同点：w 只含几何量 ⇒ 两波长 accW 相同 ⇒ R1 均安全；差别只在标度与伪影形态。

**拼接 / 全局 Σ|w| 量级对比（11 个取样点）**

```text
Σ|w|_拼接 / Σ|w|_全局 = 0.217 ~ 0.303（中心恰为 1/M = 0.125）
```

**系统性高于 1/M**：拼接保留的是与像素同扇区（角距最近）的探测器，其 d 更小、|w| 更大。
⇒ 常数 `Ω₀=4π` 归一化在两模式间会带来数倍系统性幅值偏差；`Σ|w|` 归一化不随模式变口径。

**未验证项**：`apodType=0`；`distanceWeightExponent=1.0`（改值则 R/d 不成立，需重推）；
`spliceBlendDeg=1.5°` 羽化未计入（O(blend/sector)）；多半径配准未覆盖；分层声速只影响走时不影响权重。

---

## 7. D4 图像质量度量工具

**脚本路径**：`scripts/pa_metrics.h`（工具）+ `scripts/d4_metrics_selftest.cpp`（自检）

**指标口径表（严格按任务 §3，未改口径）**

| 指标 | 定义 | 单位 | 容差/说明 |
|---|---|---|---|
| 轴向/切向分辨率 | 过目标峰值点沿**相对环心的径向/切向**取线剖，−6 dB（半高）全宽，线性插值取半高交点 | mm | −6.02 dB；环心在 (0,0) |
| 位置误差 | 重建峰值坐标与已知真值坐标的欧氏距离 | mm | 与网格步长同量级 |
| CR | `20·log10(peak / mean_background)` | dB | 背景 ROI 显式给定且不含目标 |
| gCNR | `1 − ∫min(pdf_T, pdf_B)`（Kempski 2020） | − | **bin 数 = 64**；灰度按两 ROI 合并 `[min,max]` 线性映射 |
| 背景噪声 | 背景 ROI 的 std（绝对）与相对峰值比 | 原始单位 / − | 无隐式归一化 |
| CNR | `\|mean_T − mean_B\| / std_B` | − | — |
| **双波长比值守卫** | `I1/I2` 在 ROI 内为常数（relStd ≤ 容差）且等于输入幅值比 | − | relStd ≤ 0.02、relErr ≤ 0.02 |

**附加数值口径**：`relFloor = 1e-3`（`|I2|` 低于该相对下限的像素跳过）。
这是**数值精度**保护，不是归一化——在信号为 0 的像素上做除法，比值只反映舍入误差。
跳过判据只依赖数值下限，与波长无关，故不违反 R1。

**单元自检结果：PASS 18 / FAIL 0**（退出码 0）

| # | 用例 | 结果 |
|---|---|---|
| 1 | 峰值 / 位置误差（解析高斯峰） | PASS，位置误差 0.000000 mm |
| 2 | 轴向/切向 −6 dB 宽度 vs 解析值 | PASS，轴向 0.7637 vs 解析 0.7345 mm（4 % 内） |
| 3 | CR / 背景噪声 / CNR | PASS |
| 4 | gCNR（Kempski 2020） | PASS，完全分离 ROI 得 1.000000 |
| 5 | 比值守卫**正例** | PASS，`meanRatio=2.5`（= 输入真值），`relStd=0` |
| 6 | 比值守卫**负例：逐图峰值归一化** | **确实失败**（`relErr=0.6`）← 规格指定的负例 |
| 7 | 比值守卫**负例：硬整流** | **确实失败**（`relStd` 0.049→0.242，超容差 0.12） |
| 8 | 比值守卫**负例：一侧 Hilbert 取模** | **确实失败**（`relErr=2.0`） |

⇒ **判别力成立**：3 个非线性负例都使守卫失败，而线性算子下守卫通过。

**未验证项**：未接真实重建图（属 D6/D7）；gCNR bin 数固定 64（可参数化）；
轴向/切向分辨率在目标恰在环心时未定义（已避开）。

---

## 8. D5 重建算法文献补查

**完整分析见 `D5_literature.md`。** 三条核心：

**D5-1（优先级最高）**：前提 G1 需要**收窄**。
Haltmeier, *SIAM J. Math. Anal.* **46**, 214–232 (2014)（摘要已读）给出
**任意维数**的 universal back-projection 型公式，前提是
「**未知函数支撑在该凸域内**」；一般凸域只到「＋一个平滑积分算子」，
**椭圆域（圆是椭圆特例）该算子为零 ⇒ 精确反演，且任意维数成立**。

⇒ **真正卡的不是 2D/3D，而是「源是否在探测面内」**。本项目 FOV 36 mm、
探测环 R=6.57 mm，**89.54 %** 像素在环外 ⇒ 该前提大面积不成立 ⇒ **不得作精确性声称**，
但**理由**要改写（见 D5 的措辞边界清单）。

**D5-2**：R1 的可操作判据可自验，不需文献背书；D4/D7 已实现并验证。
**「Vu 等, IEEE TMI 43(2), 771 (2024)」检索无果 ⇒ 未能核实，不得作设计依据。**
「重建算法选择对 sO₂ 误差的影响」**未找到文献**（如实记录）。

**D5-3**：**E1 推导正确**，由两条独立证据支撑（D3 数值 4.6e-16 + `RingReconInversion.h` 代数展开）。
但 **Xu & Wang 2005 原文 Eq.(22) 的字面形式未读到**（本环境取不到正文）。
另发现前提 §5 第 4 条作者列与 Crossref 不符（应为 Finch & Patch 两位）。

**检索边界（必须声明）**：本环境**只能**访问 Crossref 与 arXiv；
**没有任何一篇论文的正文被读到**。所有涉及公式/图/数值的结论均标 `未完全核实`。
这是本轮的一条交付结论：**需要可读全文的环境再补一次 D5。**

---

## 9. D6 / D7 对照基线与收益上限评估

**使用的前向模型**：见 §1.1（共享 `RingPhantomForward@6846864`）。
**D6 基线与 D7 上限评估的前向不同源**（D6 用实测数据、D7 用合成前向），
但**各自的内部自洽**：D6 的两模式对比共用同一份实测数据，D7 的 DAS/UBP 共用同一份合成前向。

### 9.1 D6 双模式指标对照（实测 testdata/01，基线 = `main` 行为 = `inversion=Das`）

```text
峰值位置（spliceMode=0）：(-9.075, -2.958) mm，值 = 91.90
目标 ROI = (峰值点, ±1.5 mm)   背景 ROI = (12, 12) mm ± 3 mm
```

| 指标 | spliceMode=0（全局） | spliceMode=1（逐通道扇区） | 差值 | 单位 |
|---|---|---|---|---|
| 轴向 −6 dB 分辨率 | 0.1409 | 0.1515 | +0.0106 | mm |
| 切向 −6 dB 分辨率 | 0.8662 | 0.1816 | −0.6846 | mm |
| CR | 38.46 | **91.37** | +52.91 | dB |
| gCNR | 0.8677 | **0.9321** | +0.0644 | − |
| 背景噪声 std | 2.413 | 1.791 | −0.622 | 原始单位 |
| 背景噪声 std/峰值 | 0.02626 | 0.00575 | −0.0205 | − |
| CNR | 7.811 | **23.753** | +15.94 | − |
| 峰值 | 91.90 | 311.57 | +219.68 | 原始单位 |

**双波长比值守卫（实测）**：**FAIL**（`relStd = 2.24` / `12.0`）—— 解释见 §3.2。

**解读限制（如实）**：本数据 **1/8 通道含信号**，故两模式差值主要反映
「单通道扇区 vs 全局」，**不是** 8 通道满孔径的有限视角效应。
全局模式下该唯一信号通道的能量被摊到 360°，扇区模式下集中在自身 45° 扇区，
故 CR/gCNR/CNR 在扇区模式下更好——这是**视角限制的方向性**证据，不是满孔径结论。

### 9.2 D7 合成 A/B（DAS vs UBP 成对，双模式，含噪声趋势）

几何：`fs=250e6, c=1490（单声速）, R=6.57 mm, fov=36 mm, sampDepth=4000`，
4000 A-line/波长（8 通道 × 500），角步长 0.09°，`sectorStart=180°`。
**与项目默认的差异（显式标注）**：`gridSize=0.1 mm`（默认 0.01 mm，计算量取舍）；
单声速（共享前向不支持分层声速，补充 §6 明令不得单速前向配分层重建）；
网格用 `ringrecon::makeGrid`（半格偏移与 CUDA 核略异）。

**场景 P（两个点目标，脉冲 Delta）——无噪**

| 模式 | 反演 | errA(mm) | errB(mm) | 轴向−6dB | 切向−6dB | A处/峰值 | B处/峰值 | 峰值半径(mm) | 环带能量占比 |
|---|---|---|---|---|---|---|---|---|---|
| global | DAS | **0.0635** | 0.0533 | 0.2372 | 0.2489 | **0.9911** | **1.0000** | 6.35 | 0.0757 |
| global | UBP | 0.8518 | 1.6096 | **0.0082** | 0.0214 | 0.0018 | 0.0053 | 16.30 | 0.0044 |
| splice | DAS | 0.0674 | 0.3722 | 0.0071 | 0.1693 | 0.0000 | 0.4927 | 3.14 | 0.0223 |
| splice | UBP | 0.1537 | 0.1754 | 0.0096 | 0.0087 | 0.0002 | 0.0019 | 15.25 | 0.0021 |

**场景 D（均匀盘，脉冲 Delta）——无噪**

| 模式 | 反演 | CR(dB) | gCNR | 背景std | CNR | 盘心/峰值 |
|---|---|---|---|---|---|---|
| global | DAS | 16.71 | **0.5256** | 3.20e6 | **1.960** | −0.0406 |
| global | UBP | 58.99 | 0.2722 | 1.37e9 | 0.033 | −0.0048 |
| splice | DAS | 21.12 | **0.6540** | 8.33e6 | **1.140** | −0.2381 |
| splice | UBP | 58.69 | 0.1030 | 2.94e9 | 0.042 | 0.0041 |

**双波长比值守卫（D7）**：**8/8 PASS**（2 脉冲 × 2 模式 × 2 反演），
`meanRatio = 2.5`（**等于已知真值**），`relStd ≈ 1e-6`。

**加噪趋势（场景 P，脉冲 Delta，SNR = 919 / 300 / 100 / 30）**

```text
SNR=919 对应 D1 实测噪声水平（噪声底 RMS≈22.4、回波峰值≈20626）。
DAS: 位置误差 0.0635 mm 在 SNR 30~919 全程稳定；背景 std 从 1.62e5 缓升到 1.86e5。
UBP: 位置误差在 0.15~0.85 mm 之间跳动，背景 std 高约 3 个数量级（1.66e8 → 3.24e8）。
     即噪声几乎不影响 UBP 的主瓣宽度，但其背景本底已远高于 DAS。
（完整 32 行表见 out_d7_phantom_ab.txt §D7.3。）
```

**网格收敛性（场景 P、Delta、global、无噪）**

| gridSize(mm) | 反演 | 位置误差A(mm) | 轴向−6dB | 切向−6dB |
|---|---|---|---|---|
| 0.200 | DAS | 7.1570 | 0.1874 | 0.1606 |
| 0.100 | DAS | 8.4965 | 0.1847 | 0.2107 |
| 0.050 | DAS | **0.0325** | 0.1195 | 0.1258 |
| 0.200 | UBP | 19.3532 | 0.0093 | 0.2188 |
| 0.100 | UBP | 19.4599 | 0.0130 | 0.0150 |
| 0.050 | UBP | 19.4305 | 0.0086 | 0.0115 |

⇒ 位置误差对网格**不收敛到 0**（UBP 稳定在 ≈19.4 mm）⇒ 那不是网格量化，
是**全局峰值落在非目标结构**上。这也是 §9.3 结论的直接证据。

### 9.3 上限评估结论

> **在理想（无噪、带宽充足）条件下，B1（UBP 成对切换）相对现有 DAS 的可改善空间
> 【体现在主瓣锐化】，但【整体图像质量指标更差】；且该结论在本项目 FOV 下成立。**

具体：

| 维度 | B1（UBP 成对）相对 DAS | 判读 |
|---|---|---|
| 主瓣宽度 | 轴向 −6 dB **0.237 → 0.008 mm**（约 29×） | **显著改善** |
| 位置误差 | 变差（0.06 → 0.85 mm，且随网格不收敛） | **变差** |
| 真值处相对幅度 | 0.991 → 0.0018 | **大幅变差**（目标不再主导图像） |
| CR | 16.7 → 59.0 dB | 数值变大，但**因目标失配而非对比度变好** |
| gCNR | 0.526 → 0.272 | **变差** |
| CNR | 1.96 → 0.033 | **大幅变差** |
| 背景噪声 std | 3.2e6 → 1.37e9 | **恶化约 400×** |
| 双波长比值守卫 | 8/8 PASS | **不劣化（R1 安全）** |
| 有限视角（splice vs global） | 两种模式结论同向 | 结论稳健 |

**不预设优劣的如实陈述**：UBP 在**主瓣锐化**上确实有收益，
但在**真值可辨识性 / gCNR / CNR / 背景噪声**上更差。
原规格 §1 明确「**D7 不是闸门**（即使收益有限也照做 B1）」，故本结论只用于预判投入产出。

**根因（与 D3 / D5 交叉吻合）**：FOV 36 mm 中 89.54 % 像素在探测环外；
精确反演要求源支撑在探测面内（Haltmeier 2014）；UBP 的 `1/d²` 权重在环附近按
`(R/minDist)² = 431649×` 放大（D3）。全局峰值稳定落在 r≈16–19 mm（非目标、非环带），
说明主导的是**域外/边界结构**而非近环奇点（D3 的环带能量占比：UBP 0.4 % < DAS 7.6 %，
**不支持**「UBP 被近环奇点主导」这个假设——如实记录该假设未被证实）。

**未验证项 / 限制**
- 前向不建模折射/透射、**分层声速**、有限探头尺寸、噪声（D7.3 单独加噪）、量化、fluence（补充 §6）。
- 本结果是**模型自洽**的合成 A/B，不是实机效果预估。
- 绝对量级不可比（补充 §4）：UBP 的 `b=2p−2t·p′` 量纲与 DAS 的 `p` 不同；
  可比的是空间分布、相对幅度与质量指标。
- `gridSize` 非项目默认 10 µm。
- **D6 与 D7 的前向不同源**（实测 vs 共享合成），两者的绝对数值**不可混比**。

---

## 10. 证据口径（四层分离，逐任务）

| 任务 | source/code correctness | automated tests/build/selftest | real hardware validation | hardware root-cause attribution |
|---|---|---|---|---|
| D1 | 未声称 | 脚本可复跑 = 是 | testdata/01 + 14.dat（历史实测，provenance 未确认）；非合成，但非本轮验收数据 | 未声称 |
| D2 | 未声称 | 构建成功 + 基准可复跑 = 是（附命令） | **仅真实 GPU 性能实测，≠ 实机采集验证** | 未声称 |
| D3 | 未声称 | 脚本可复跑 = 是 | 不适用（纯几何/数值） | 未声称 |
| D4 | 是（工具实现正确性，18/18 自检） | 自检可复跑 = 是 | 不适用 | 未声称 |
| D5 | 未声称 | 不适用 | 不适用（纯文献） | 未声称 |
| D6 | 未声称 | 脚本可复跑 = 是 | testdata/01（历史实测） | 未声称 |
| D7 | 未声称（前向正确性见其 25/25 契约测试；生产重建只作被测对象） | 脚本可复跑 = 是 | **合成数据** | 未声称 |

**全任务共同**：不把软件 PASS 写成实机 PASS；不把 `7/4007` 当协议常量。

---

## 11. 执行过程中的一次工作丢失（必须记录）

本轮早段（约 04:00 UTC+8）曾完成 D2/D4/D5/D6/D7 并提交到本分析分支
（SHA `4712f2f6…`）。随后**该提交从对象库中消失**（`git cat-file` 报
`could not get object info`，`git fsck --lost-found` 未找到），
分析分支被移回 `65de782`。同仓有**另一个执行代理**在
`codex/das-dual-wavelength-quality-b-tier-20260925` 上并行工作（reflog 可见）。

- 仅 D1/D3 的脚本与输出在工作区留存，已备份到 `_task_work/BACKUP-btier-113832/`；
- D2/D4/D5/D6/D7 已按本轮**重做**（且 D6/D7 已按补充规格改用共享前向）；
- 本轮提交后**立即 push 到远端**，`local HEAD == remote HEAD`，以防再次丢失。

**给规划侧的建议**：并行代理共用同一工作区时，
「reset/gc 可能吞掉他人已提交的工作」应作为流程风险记录。

---

## 12. 给规划侧的三条建议

1. **R1 的实机验收判据要改**：比值守卫不能对实测图断言 PASS（见 §3.2）；
   应改为对**已知比例的输入**断言。D4/D7 已提供该用法与判别力证明。
2. **B2 在本数据上不建议实施**（D1 §5）：fc 要压到 ≤2 MHz 才保得住微分 SNR，
   但信号带宽 53.4 MHz，轴向分辨率会差 26.7 倍。若必须走 B1，
   优先考虑**不带导数项的权重改造**（保留 `p` 的信号项）。
3. **B1 的预算不是瓶颈，FOV 才是**（D2/D7）：预算余量 98.74 %，
   但 UBP 在本项目 FOV 下被域外结构主导。若要做 B1，
   应先定「目标像素在环内还是环外」的成像口径（例如把有效 FOV 收到环内，
   或按 D3 的候选 C 做环外掩膜），再谈算法收益。
