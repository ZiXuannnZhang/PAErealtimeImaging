# 光声重建增强算法调研：UBP 与 FBP

> 日期：2026-09-25
> 目的：在已完成环形扫描 DAS 重建的基础上，调研**滤波反投影（FBP）**与**光声通用反投影（UBP）**两类候选增强算法，为后续方案设计提供依据。本文档**仅调研，不含算法方案设计**。
> 可信度声明：所有引用文献均于 2026-09-25 经 Web 检索 / Europe PMC / 出版方页面核实题录与内容；**标注"未完全核实"或"间接核实"的条目不能作为方案设计依据**。报告中未提供直接文献证据的对比结论均明确标注为"无直接文献证据"或"结构推断"，未对任何算法在项目数据上的增强效果做预估。

---

## 0. 结论速览

| 维度 | UBP（通用反投影） | FBP（滤波反投影） |
|---|---|---|
| 与现有 DAS 实现的关系 | 同为逐 A-line 无状态反投影，改造点集中在**信号域积分项**（2p−2t·p′）与**权重语义**；MATLAB 参考实现的默认权重已与圆环 UBP 权重同形，且代码注释已预留 UBP 积分项接口 | 同为反投影族，前置**逐 A-line 时域滤波**；滤波可预计算为矩阵（2025 年已有 GPU 超快实现先例），反投影核需修改权重 |
| 增量分块流水线兼容 | 完全兼容（逐 A-line 独立，无跨块依赖） | 滤波在单根完整 A-line 内完成，无块间依赖，理论兼容；滤波参数需固定化 |
| 几何假设 | "universal"：权重按每个探测器位置独立计算，天然适配逐通道半径配准模式 | 经典圆周公式要求所有探测器位于**同一圆**、恒定声速；与逐通道半径配准模式存在冲突 |
| 双波长 sO2 | 不阻碍比值法；单极输出对逐像素比值更友好（结构推断）；声速/带宽假设同样适用 | 同左；滤波核的低频行为对定量信息的影响需在设计阶段评估（Vu 2024） |
| 性能负荷（文献锚点） | ≈ DAS + 一次导数预处理 + 时间加权，同阶复杂度 | DAS + 逐 A-line 滤波（FFT 或矩阵乘）；512×512 图像 0.38 ms（RTX 3080，Liu 2025） |
| 文献直接对比 | "FBP vs UBP"的系统直接对比文献**很少**（详见 §4.1），两条算法线在近年文献中更多作为独立演进方向存在 | |

---

## 1. 调研背景：项目现状（来自工作区代码与文档）

以下事实来自 `PAErealtimeImaging-impl` 工作区，是兼容性分析的基础：

- **production 实现**：C++/CUDA（`MC_410T_MultiCard/delivery/ring_recon_cuda.dll`，CUDA 12.8，Qt 6.8，实测 GPU 为 RTX 4060 Laptop 8 GB）。
- **采集**：环形扫描，250 MHz 采样，采样时间 50 µs（每根 A-line 12,500 点，完整记录），采样重频 40 Hz；每圈 8,000 条 A-line（**双波长交替**，每波长 4,000 条）。
- **重建网格**：40 mm × 40 mm @ 10 µm（4000×4000 = 1,600 万像素）；FOV 扇区可选。
- **增量流水线**：分块 append（逐块把 A-line 贡献累加进图像累加器）+ snapshot（归一化出图）；基准报告给出 append 3.9–213.7 ms、snapshot ≈12.2 ms（视通道数/块大小，见 `重建核心基准测试报告.md`）。
- **几何特性**：支持**逐通道半径**（multiRadius 配准模式，各通道半径可不同）与**同心分层声速**（`SoundSpeedRadii`/`SoundSpeeds`）；拼接模式有 1.5° 羽化。
- **MATLAB 参考实现**（`实时重建脚本/das_recon_circular_gpu_v2.m`）两个关键事实：
  1. 权重形式 `w ∝ (r·r_s − R²)/(R·|r−r_s|²) · arc · apodization`（默认 `DistanceWeightExponent=1`），这正是圆环几何 UBP 的反投影权重形式（探测器内法向与像素方向夹角余弦 / 距离）；
  2. 函数文档已明确预留 UBP 接口：*"For universal back-projection use the integrand bp = 2*p - 2*t*dp/dt"*。
- **现有 DAS 后处理选项**：相干因子（CF）、Hilbert 包络、变迹（Hann/Hamming）、角度调制、FOV 掩膜、`NormalizeByW`（按累计 |w| 归一化）。

---

## 2. UBP（Universal Back-Projection）调研

### 2.1 原理

- **基础文献**：M. Xu & L. V. Wang, "Universal back-projection algorithm for photoacoustic computed tomography," *Phys. Rev. E* 71, 016706 (2005)，DOI: [10.1103/PhysRevE.71.016706](https://doi.org/10.1103/PhysRevE.71.016706)（被引 >1700 次，据检索结果）。
- **思想**：针对平面、球面、柱面（含圆环）等闭曲面检测几何，给出统一的时域反投影公式：对每路信号取积分项（圆/球/柱几何下实现通式为 `2p(t) − 2t·p′(t)`，t 以秒计），在 t = |r₀−r_s|/c 时刻取值后乘以权重 β（与探测器表面法向、像素—探测器方向点积及距离有关）反投影累加。与普通 DAS/BP 的差别有两点：**信号域**用导数加权积分项替代原始压力，**权重域**用 β 替代经验权重。
- **输出物理量**：初始声压梯度的反演量，对应**吸收能量密度（单极、非负物理量）**，而不是普通 BP 的双极压力像。这是 UBP 相对 DAS 在"单极性"上的根本区别。
- **普适性**：对任意（闭）检测曲面成立——权重逐探测器独立计算，不要求同一圆周。

### 2.2 已知性质与问题（文献证据）

- **理想条件**：无限带宽点探测器、闭曲面全视角、恒定声速下公式精确（Xu & Wang 2005）。
- **负值伪影两大根源 = 有限带宽 + 有限视角**：K. Shen, S. Liu, T. Feng, J. Yuan, B. Zhu, C. Tian, "Negativity artifacts in back-projection based photoacoustic tomography," *J. Phys. D: Appl. Phys.* 54, 074001 (2021)（2020-11 在线）。带宽不足时正信号旁出现负下冲；有限视角使负值无法被其他方向贡献抵消。**该结论对整个反投影族（含 DAS/UBP）成立**。
- **有限视角下的量化表现**（工作区文献）：Hakakzadeh 等 IUS 2022 数值实验中，半视角圆阵 UBP 的 gCNR = 0.64（作为其单极化方法的对比基线）。
- **声速失配**：UBP 公式假设恒定声速；分层声速/声速不确定性会使重建退化。相关近年工作：Y. Sahbaz 等 (2024, ACM，间接核实) 研究分层声速不确定性下的两层 UBP；D. Jiang 等, "Signal-domain speed-of-sound correction for ring-array-based photoacoustic tomography," *Photoacoustics* (2025), DOI: 10.1016/j.pacs.2025.100735——环形阵列声速校正仍是活跃问题（该文报道实时双声速 10 fps，属检索摘要信息）。

### 2.3 近五年进展脉络（2020–2025）

- **作为对比基线与被改进对象**：近年主流改进方向是深度学习、模型类迭代与非负约束，UBP 因"快且物理意义明确"被普遍选为基线。例：I. Steinberg 等, "Superiorized Photo-Acoustic Non-NEgative Reconstruction (SPANNER) for Clinical Photoacoustic Imaging," *IEEE Trans. Med. Imaging* 40(7):1888–1897 (2021), DOI: 10.1109/TMI.2021.3068181——SPANNER 与 **DAS、UBP、MBR（模型类）** 三者对比，仿真/仿体/临床前列腺数据中对比度提升最高达 **+20 dB**（摘要核实）。这说明 UBP 并非有限视角问题链的终点。
- **几何/系统修正类扩展**：
  - B. Wang 等, "Back-projection algorithm in generalized form for circular-scanning-based photoacoustic tomography with improved tangential resolution," *Quant. Imaging Med. Surg.* 9(3):491–502 (2019), DOI: 10.21037/qims.2019.03.12——用"虚拟探测器"（置于优化距离处的等效点）近似有限尺寸平面探头的时空响应，改善切向分辨率；与环形扫描几何直接相关（探头有限尺寸是本项目实际存在的因素）。
  - 经颅分层 UBP（L-UBP，Na 等 2020，**出处细节未完全核实**，经 Zhang 2023、Liang 2021 等多篇引文间接确认存在）：把 UBP 权重按分层声速折射/反射修正。
- **与工作区文献的关系**：Hakakzadeh 等 IUS 2022（工作区已有一篇）与 UBP 同属"单极化反投影"思路，但技巧在信号域（求导翻转 + λ 斜率补偿 + 双图求和整流），并把 UBP 作为对比方法之一。可视为 UBP 的同族竞争路线。

---

## 3. FBP（Filtered Back-Projection）调研

### 3.1 原理

- **基础文献（PAT 圆周几何经典）**：M. Xu & L. V. Wang, "Pulsed-microwave-induced thermoacoustic tomography: Filtered backprojection in a circular measurement configuration," *Med. Phys.* 29(8):1661–1669 (2002), DOI: [10.1118/1.1493778](https://doi.org/10.1118/1.1493778)——圆周检测配置下基于严格反演理论的滤波反投影。
- **思想**：先对每路时域信号做频域滤波（ramp 类滤波核，物理上来自波动方程反演中的频率因子），再反投影。滤波把双极压力信号变换为与吸收能量密度对应的有效波形，因此在理想条件下 FBP 与 UBP 都指向"吸收能量密度"这一单极量（两者的数学根源同为球面平均反演，具体形式随几何不同）。
- **滤波器设计的自由度**：ramp 核需加窗（带限化）以控制噪声放大；截止频率与窗型是 FBP 特有的系统参数。近年工作：M. Beckmann 等, "Optimized filter functions for filtered back projection"（2024 arXiv / 2025 期刊，**期刊出处未完全核实**，aimsciences.org 检索命中）——解析推导最小化重建误差的滤波函数。

### 3.2 近五年进展脉络（2020–2025）

- **超快 GPU FBP（最重要锚点）**：S. Liu, Z. Tan, P. Shao, S. Wang, C. Tian, "Ultrafast filtered back-projection for photoacoustic computed tomography," *Biomed. Opt. Express* 16(2):362–381 (2025), DOI: [10.1364/BOE.540622](https://doi.org/10.1364/BOE.540622)（Europe PMC 全文核实）：
  - 方法：把滤波核（升余弦低通 + iω 微分项）预计算为**单个时域滤波矩阵**（循环矩阵），运行时一次矩阵乘完成滤波（cuBLAS），无需运行时 FFT/IFFT；反投影 GPU 并行。
  - 性能：RTX 3080 上 512×512 二维图像 **0.38 ms**，比 MATLAB 实现快 439×、比 C++ CPU 快 163×；全部 2D 场景 <1 ms、<10 MB 显存。
  - 假设/限制（全文核实）：演示几何为 2D 圆环（256 阵元，半径 25–50 mm）与 3D 球阵；滤波矩阵绑定采样率/采样长度/滤波类型/截止频率（改动需重算）；假设恒定声速；传播时延取最近整数样点（最近邻插值）；`use_fast_math` 带来约 3–3.5% GPU/CPU 差异；**明确指出有限视角产生负值伪影**。
  - 注意：该文的对比对象是三种既有 GPU FBP 变体（RMSE/PSNR），**未与 UBP/DAS/TR 直接对比**。
- **自适应/加权 FBP 变体**（>5 年，作为脉络补充）：H. Huang 等, "An adaptive filtered back-projection for photoacoustic image reconstruction" (2015, Med. Phys./PMC)——用权重函数充当频域 ramp 滤波；S. Liu 等, "Regularized iterative weighted FBP" (2016, PMC)。
- **综合评述**：Zhang S., Miao J., Li L. S., "Challenges and advances in two-dimensional photoacoustic computed tomography: a review," *J. Biomed. Opt.* 29(7):070901 (2024), DOI: 10.1117/1.JBO.29.7.070901——把**有限视角、各向异性分辨率、空间欠采样、声学异质性、fluence 校正**列为 2D PACT 的五大挑战（题录+主题核实），可作为判断"任何解析算法在真实系统中的天花板"的依据。

### 3.3 FBP 对系统的隐含要求（结构性事实，非效果预估）

1. 探测器近似点探测器且位于**同一**圆周/闭曲面上（经典公式）；
2. 恒定声速（或分层扩展，需专门推导）；
3. 全视角时解析精确；**有限视角下精确反演条件不成立**，算法退化为近似（与所有解析法相同）；
4. 角向采样足够密（本项目每波长 4,000 条/圈，远超常见演示的 256 阵元，此项不构成约束）。

---

## 4. 两类算法的成像质量比较（仅汇总文献证据）

### 4.1 直接对比文献稀少——如实说明

检索范围内**未找到以"FBP vs UBP"为主线的系统对比论文**。近年两篇大规模算法评估各自覆盖了一半：
- Prakash R., Manwar R., Avanaki K., "Evaluation of 10 current image reconstruction algorithms for linear array photoacoustic imaging," *J. Biophotonics* 17(3):e202300117 (2024), DOI: 10.1002/jbio.202300117——对比 **DAS、UBP、pDAS、DMAS、MV、EIGMV、SLSC、GSC、TR、FD（频域法）** 十种算法（体外仿体；指标：横向分辨率、耗时、目标可探测性、噪声敏感性）。据出版方页面摘录："横向分辨率以 DAS 与 UBP 最高（pDAS 紧随第三）"；全文（Wiley，CC BY-NC-ND）付费墙内，未能核实其余排名。
- Park H., Vu T., Yao J., Jing Y., "A comparison of image reconstruction methods for ring array photoacoustic computed tomography," *IEEE Trans. Ultrason. Ferroelectr. Freq. Control* 72(10):1376–1387 (2025), DOI: 10.1109/TUFFC.2025.3600372——**环形阵列**（512 元、半径 40 mm、5 MHz 中心频率、0.1–7 MHz 带宽）上对比 **DAS、IMMI（迭代最小范数）、FDMB（频域模型类）、TR（时间反演）**：DAS 最快（101×101 网格 GPU 0.30–0.45 ms，RTX 4090）但存在**负值伪影与有限视角伪影**；模型类方法伪影更少但慢一个数量级以上（GPU 15–450 ms）；TR 最慢（约 4 s）。该文未含 FBP/UBP。

即：**两类算法分别出现在不同的对比研究集合里**，公开文献没有给出二者在同一数据集上的直接量化对比。这一点在设计阶段需靠本项目自建基准弥补。

### 4.2 可用于推断质量差异的已核实证据链

1. **双极性/负值**：反投影族（DAS/UBP）都受负值伪影影响，根源=带宽+有限视角（Shen 2021）。FBP 的滤波步骤天然带通，负值表现为滤波 ringing 而非 N 形双极伪影（结构推断，无直接文献量化）。
2. **单极化与幅度一致性**：UBP 与 FBP 在理想条件下都重建吸收能量密度（单极量）；普通 DAS/BP 重建双极压力像。工作区文献 Hakakzadeh 2022 给出半视角 gCNR 锚点：UBP 0.64，其单极化方法 0.96（该方法非 FBP）。
3. **带宽敏感性**：UBP 积分项含导数项，对带限信号更敏感（Xu & Wang 2005 原文即讨论了低通滤波处理；Shen 2021 系统化了带宽→负下冲机理）；FBP 的带限窗是显式设计参数。**两者对带宽的处理机制不同，但文献未给出敏感性高低的直接对比**。
4. **上限参照**：SPANNER 2021（+20 dB vs DAS/UBP/MBR）与 Park 2025（模型类/TR 伪影更少）共同说明：解析反投影类（含 UBP/FBP）之上仍有质量空间，代价是计算量。

---

## 5. 与当前项目 DAS 实现方式的兼容性

> 本节为基于工作区代码事实的工程分析，非文献结论。

### 5.1 UBP：改造成本最低的路径

| 改造点 | 现状 | UBP 所需改动 |
|---|---|---|
| 信号域积分项 | 输入原始压力 p | 预处理生成 `2p − 2t·p′`（t 秒；导数离散格式需选定）。每根 A-line 独立、12,500 点完整记录内完成，**无跨块依赖** |
| 权重 | `w ∝ (r·r_s−R²)/(R|r−r_s|²)·arc·apod`，与圆环 UBP 权重同形 | 基本不动（可能需按 Xu & Wang 2005 校核常数与符号约定） |
| 归一化 | `NormalizeByW`（按累计 \|w\| 归一化） | UBP 语义下**不应**按权重和归一化（β 是公式权重而非求和权重）——需绕过或重定义 |
| 增量 append/snapshot | 无状态逐 A-line 累加 | 完全兼容 |
| 逐通道半径配准 | 已支持 | 天然兼容（β 逐探测器计算，"universal" 的含义） |
| 分层声速 | 飞行时间已支持分层 | UBP 公式假设恒定声速，分层下公式需扩展（开放问题；可参照 L-UBP 思路，**出处未完全核实**） |
| 双波长交替 | ShiftWL2 逐 A-line 错位 | 导数逐 A-line 独立，不引入跨波长依赖；首块语义需按 production 约定确认 |
| 后处理链 | CF / Hilbert 包络 / FOV 掩膜 / 羽化 | UBP 输出为吸收能量密度量纲，**是否保留包络/CF 的语义需重新定义**（设计阶段决策点） |

### 5.2 FBP：需要新增滤波级与权重修改

| 改造点 | 说明 |
|---|---|
| 滤波级 | 逐 A-line 时域滤波；参照 Liu 2025（BOE 2025）预计算滤波矩阵 + 单次矩阵乘的 GPU 路线，与项目现有 CUDA 卷积/矩阵基础设施契合。滤波在完整 A-line（12,500 点）内进行，**无跨块依赖** |
| 反投影核 | 权重与 UBP/DAS 不同（含频率因子带来的 1/t 类修正项），需新核或参数化现有核 |
| 几何假设冲突 | 经典圆周 FBP 要求所有探测器同一半径；与项目**逐通道半径配准模式**直接冲突。可行方向（仅列出，不展开）：等效圆近似 / 按通道修正权重——**均属设计阶段工作** |
| 分层声速 | 同 UBP：经典公式假设恒定声速 |
| 参数固化 | 滤波类型/截止/窗、采样率、采样长度改变时需重算滤波矩阵（Liu 2025 明确此约束）；250 MHz / 12,500 点为固定采集参数，固化是可行的 |
| 插值 | Liu 2025 用最近邻取值；项目现有核已支持线性插值，滤波后信号插值方式对精度的影响需实测 |

### 5.3 共同注意点

- 两类算法**都不改变**增量流水线的复杂度阶：反投影仍是 O(N_lines × N_pixels) 主导项。
- 现有 A/B/C/D 验证链与数值对照（MATLAB reference ↔ CUDA）可作为新算法的回归基线。

---

## 6. 双波长血氧（sO2）计算兼容性

1. **流程正交性**：sO2 = 对两个波长各自重建的图像做 HbO₂/HbR 光谱解混（比值法）。重建算法属于声学域，波长比属于光学域，**两者在流程上正交**——FBP/UBP 不阻碍双波长比值计算。
2. **单极性对逐像素比值的意义**：普通 DAS 双极图像在负值/过零像素处比值不稳定甚至无意义；UBP/FBP 理想条件下输出单极的吸收能量密度，对逐像素比值更友好（**结构推断**；文献侧佐证：Hakakzadeh 2022 与 Li 等 MVHT 2017 的动机均为"双极性使 sO2/MRO2 定量困难"）。
3. **共同瓶颈一：fluence**：波长依赖的光注量分布不随声学重建算法改变，是双波长定量精度的根本限制之一（JBO 2024 综述将 fluence 校正列为 2D PACT 五大挑战之一）。
4. **共同瓶颈二：低频信息**：Vu T., Klippel P., … Yao J., "On the Importance of Low-Frequency Signals in Functional and Molecular Photoacoustic Computed Tomography," *IEEE Trans. Med. Imaging* 43(2):771–783 (2024), DOI: 10.1109/TMI.2023.3320668——低频 PA 成分"承载功能与分子信息，可用于提升定量精度"。含义：**任何增强算法不应丢弃低频成分**。UBP 积分项中的导数项与 FBP 的带限滤波核各自的低频行为，是设计阶段需要显式评估的点（本文档不预断其影响）。
5. **无直接文献证据**：检索未发现"FBP vs UBP 对双波长 sO2 定量误差影响"的直接对比研究。

### 6.1 补充分析：文献式归一化对 sO2 的影响与"去归一化"的可行性

> 追加分析（2026-09-25），针对文献 [16]（Hakakzadeh 等 IUS 2022）六步流程中的归一化步骤。除特别注明外，本小节为**分析性结论**（基于算法结构的数学性质），非文献报道结果。

**文献中归一化步骤的定位**。文献 [16] 第 5 步为 *"Normalizing both the initial and secondary reconstructed images and then summing them together"*——对**同一波长内**的两幅分量图（初始双极图 BP{p} 与导数补偿图 BP{−λ·p′}）各自归一化后相加，第 6 步再取正整流。正文未给出归一化公式；且该文献仅做点目标/Derenzo 仿体仿真，**没有任何双波长或 sO2 实验**，故其归一化只是"两幅图配平后可相加"的算法内部步骤，未考虑跨波长一致性。

**数学要点**。比值法 sO2 只依赖两波长重建量之比 R = I(λ₁)/I(λ₂) ∝ μa(λ₁)/μa(λ₂)（解混公式对 R 非线性单调映射到 sO2）。关键性质：**对两波长完全相同的标度因子在比值中相消；波长间不一致的标度因子不相消**。

**影响分析**：

1. 归一化因子是**内容性**的，且逐波长独立产生。两波长的光注量分布与血红蛋白吸收幅度不同 → 图像内容不同 → 归一化因子一般不相等，且不可预知、不可后验校正。
2. 后果是**全局乘性偏差**：实测比值 R′ = R·(γ₁/γ₂)，γ_w 为该波长归一化因子的等效合成；经解混公式映射后得到系统性偏移的血氧值。偏差大小由图像内容决定。
3. **帧间不稳定**：图像内容一变（血管出视场、运动、光漂白、最强吸收体变化），归一化因子即变；真实 sO2 恒定时计算值仍会漂移。
4. **与整流步骤（第 6 步取正）的交互**：整流是逐像素非线性。两波长配平越好，求和后残余负值越少、被裁像素越少；归一化不当会放大残余负值，导致某波长在该像素被裁为零而另一波长未被裁，比值在这些像素**局部失效**（空洞或错误值）。
5. 需要区分：两幅分量图点扩散函数不同（导数图偏高通）造成的比值空间畸变是"双图求和"方法**固有的**，与归一化无关；归一化额外引入的是第 1–3 条的全局标度问题。

**是否"完全破坏"血氧计算**：

- 若归一化是**常数且两波长严格一致**（或干脆去掉）：常数在比值中相消，sO2 **完全不受影响**——比值法本来不依赖绝对幅度。
- 若按文献做法**逐图像、按内容归一化**：空间相对结构与 sO2 的单调性仍保留（全局标度，非逐像素破坏），但**绝对血氧值不可信且随帧漂移**。定量语境下应视为不可用——"破坏定量能力"成立，"完全破坏"不准确。

**去掉归一化后是否仍是滤波反投影**：**能，且是该方法更自然的形式。** 归一化不是文献方法中的滤波环节——滤波是第 2、3 步（求导、翻转、乘 λ）。去掉第 5 步后：

$$I = \mathrm{BP}\{p\} + \mathrm{BP}\{-\lambda(t)\,p'\} = \mathrm{BP}\{p - \lambda(t)\,p'\}$$

即"固定线性滤波（积分项 p − λ(t)·p′）→ 反投影 → 整流"的结构。三个要点：

1. **λ(t) 必须保留且固定**：它承担 p′（Pa/s）与 p（Pa）的量纲换算（λ 量纲为秒）及负斜率补偿。去掉归一化后两图相对幅度由物理量决定，负值补偿是否充分取决于 λ 的标定——正确做法是**用固定的、物理标定的增益（λ 及一个常数）替代内容性归一化**，而非简单删除。
2. **两波长必须使用严格相同的 λ(t) 与增益**：满足则整个算子对两波长一致，比值法不被标度破坏。λ 为时变函数无妨——确定且跨波长一致即可。
3. **结构推论（连接 §2 与 §5.1）**：取线性时变形式 λ(t) ∝ t 并等权相加时，积分项 p − t·p′ 与 UBP 积分项 2p − 2t·p′（文献 [1]；项目 MATLAB 参考 [17] 已预留该接口）**严格同构**（差常数因子）。即文献方法去归一化、λ 取线性形式后收敛到 UBP 公式族，而 UBP 重建的单极吸收能量密度恰为文献的目标输出。

**落地含义**：项目现有 `NormalizeByW` 是**几何性归一化**（按累计权重和除，只依赖采集几何，两波长相同），对血氧比值安全；文献式**内容性归一化应避免**。若将文献思路改造为"固定 λ(t) + 等权（或固定权）相加"的滤波反投影/UBP 形式，血氧兼容性优于文献原始流程；代价是 λ 的标定工作量，以及残余负值经整流在局部像素的固有影响（有无归一化均存在）。

---

## 7. 性能负荷

**文献锚点（均已核实）**：

| 方法 | 硬件 | 规模 | 耗时 | 出处 |
|---|---|---|---|---|
| DAS（环阵） | RTX 4090 | 101×101 网格 | 0.30–0.45 ms/帧 | Park 2025, IEEE TUFFC |
| 频域模型类 FDMB | RTX 4090 | 同上 | 15–450 ms | Park 2025 |
| 时间反演 TR | — | 同上 | ≈4 s | Park 2025 |
| 超快 FBP | RTX 3080 | 512×512 | 0.38 ms（滤波矩阵预计算 + cuBLAS） | Liu 2025, BOE 16(2) |
| GPU 加速压缩感知 PACT | GPU | — | 比 CPU 快 24–31×（体内手部成像） | Gao 等 2020（检索摘要，未逐项核实） |

**复杂度结构（结构性分析）**：
- DAS/UBP：O(N_lines × N_pixels)；UBP 增量 = 一次长度 N_t 的导数（每 A-line O(N_t)）+ 时间加权查表，相对反投影可忽略。
- FBP：O(N_lines × N_t·logN_t 或 N_t²矩阵乘) 滤波 + 同阶反投影。滤波项与反投影项相比通常为小量；矩阵化滤波后无运行时 FFT。
- 本项目规模（4000×4000 像素、8000 A-line/圈、双波长增量分块）远大于文献演示规模，**文献耗时数字不能直接外推**；项目已有基准（append 3.9–213.7 ms、snapshot ≈12.2 ms）可作为改造后 A/B 实测的对照基线。**本报告不对改造后的帧率做预估。**

---

## 8. 调研结论（仅事实与兼容性判断，不含方案）

1. **UBP 是与现有 DAS 框架同构的最小改造路径**：信号域积分项（项目 MATLAB 参考已预留接口）+ 权重语义调整（关闭 NormalizeByW）；逐通道半径配准天然兼容；增量流水线零改动。风险点：带限/有限视角下的负值残留（Shen 2021 机理；Hakakzadeh 2022 半视角 gCNR 0.64 锚点）、分层声速下公式需扩展。
2. **FBP 是一条独立的解析路径，工程质量上限受系统假设约束**：同一圆、恒定声速、全视角时精确；与逐通道半径配准模式存在公理性冲突，需要设计阶段解决。GPU 实时性已被 2025 年文献证明（0.38 ms@512²），且预计算滤波矩阵路线与项目 CUDA 基础设施契合。
3. **两者对双波长 sO2 均无流程性障碍**；相对普通 DAS 的理论收益方向是单极性（利于逐像素比值），但受 fluence 与低频信息保留问题制约，且**无直接文献证据量化二者对 sO2 的影响差异**。
4. **文献中缺少 FBP vs UBP 的同数据集直接对比**；两篇近年大规模评估（Prakash 2024 线阵、Park 2025 环阵）各覆盖一条线，不能拼出直接结论。后续若进入设计阶段，应以本项目自建基准（k-Wave 仿体 + 现有重建基准脚本）做同数据 A/B。

### 设计阶段需要回答的问题清单（留待下一步，不在本文档展开）

- 导数积分项（UBP）与滤波核（FBP）在 250 MHz / 66% 量级带限信号下的具体表现；
- UBP 关闭 NormalizeByW 后动态范围/显示链路的适配；
- FBP 与逐通道半径配准、分层声速的公式改造取舍；
- 单波长双极→单极转换对现有 CF/包络/羽化后处理链的语义影响；
- 去归一化改造中补偿函数 λ(t) 的形式选择与标定（含 λ(t) ∝ t 时与 UBP 积分项同构的验证，见 §6.1）；
- 与 Hakakzadeh 2022 单极化方法（工作区文献）的对比实验设计。

---

## 9. 参考文献清单

**核心经典**
1. M. Xu, L. V. Wang, "Universal back-projection algorithm for photoacoustic computed tomography," *Phys. Rev. E* 71, 016706 (2005). DOI: 10.1103/PhysRevE.71.016706.（已核实）
2. M. Xu, L. V. Wang, "Pulsed-microwave-induced thermoacoustic tomography: Filtered backprojection in a circular measurement configuration," *Med. Phys.* 29(8):1661–1669 (2002). DOI: 10.1118/1.1493778.（已核实）

**近年关键文献（2020–2025）**
3. S. Liu, Z. Tan, P. Shao, S. Wang, C. Tian, "Ultrafast filtered back-projection for photoacoustic computed tomography," *Biomed. Opt. Express* 16(2):362–381 (2025). DOI: 10.1364/BOE.540622.（Europe PMC 全文核实，PMC11828441）
4. H. Park, T. Vu, J. Yao, Y. Jing, "A comparison of image reconstruction methods for ring array photoacoustic computed tomography," *IEEE Trans. Ultrason. Ferroelectr. Freq. Control* 72(10):1376–1387 (2025). DOI: 10.1109/TUFFC.2025.3600372.（PMC 页面核实）
5. R. Prakash, R. Manwar, K. Avanaki, "Evaluation of 10 current image reconstruction algorithms for linear array photoacoustic imaging," *J. Biophotonics* 17(3):e202300117 (2024). DOI: 10.1002/jbio.202300117.（题录+摘要核实；结论摘自出版方页面片段，全文付费墙内）
6. I. Steinberg et al., "Superiorized Photo-Acoustic Non-NEgative Reconstruction (SPANNER) for Clinical Photoacoustic Imaging," *IEEE Trans. Med. Imaging* 40(7):1888–1897 (2021). DOI: 10.1109/TMI.2021.3068181.（Europe PMC 题录+摘要核实）
7. K. Shen, S. Liu, T. Feng, J. Yuan, B. Zhu, C. Tian, "Negativity artifacts in back-projection based photoacoustic tomography," *J. Phys. D: Appl. Phys.* 54, 074001 (2021).（检索核实）
8. T. Vu et al., "On the Importance of Low-Frequency Signals in Functional and Molecular Photoacoustic Computed Tomography," *IEEE Trans. Med. Imaging* 43(2):771–783 (2024). DOI: 10.1109/TMI.2023.3320668.（Europe PMC 题录+摘要核实）
9. B. Wang, T. Su, W. Pang, N. Wei, J. Xiao, K. Peng, "Back-projection algorithm in generalized form for circular-scanning-based photoacoustic tomography with improved tangential resolution," *Quant. Imaging Med. Surg.* 9(3):491–502 (2019). DOI: 10.21037/qims.2019.03.12.（Europe PMC 题录核实）

**综述**
10. S. Zhang, J. Miao, L. S. Li, "Challenges and advances in two-dimensional photoacoustic computed tomography: a review," *J. Biomed. Opt.* 29(7):070901 (2024). DOI: 10.1117/1.JBO.29.7.070901.（Europe PMC 题录核实）
11. D. Jiang et al., "Signal-domain speed-of-sound correction for ring-array-based photoacoustic tomography," *Photoacoustics* (2025). DOI: 10.1016/j.pacs.2025.100735.（题录核实，细节未读全文）

**脉络补充（未完全核实/较旧）**
12. M. Beckmann et al., "Optimized filter functions for filtered back projection," arXiv 2024 / 期刊 2025（aimsciences.org 检索命中，期刊出处未完全核实）。
13. H. Huang et al., "An adaptive filtered back-projection for photoacoustic image reconstruction," (2015).（检索核实存在，作为 FBP 变体脉络）
14. S. Na et al., 分层 UBP（L-UBP，经颅 PAT，2020）——**出处细节未完全核实**，经多篇引文间接确认。
15. Y. Sahbaz et al., 分层声速不确定性下的两层 UBP (2024, ACM)——**间接核实**。

**工作区文献**
16. S. Hakakzadeh, S. M. Mostafavi, Z. Kavehvash, "Unipolar Back-Projection Algorithm for Photoacoustic Tomography," 2022 IEEE IUS. DOI: 10.1109/IUS54386.2022.9957429.（本地全文阅读）

**项目内部依据**
17. `PAErealtimeImaging-impl/实时重建脚本/das_recon_circular_gpu_v2.m`（权重形式与 UBP 接口预留）。
18. `PAErealtimeImaging-task-docs/重建核心基准测试报告.md`（2026-08-16，CUDA 重建基准数据）。
19. `PAErealtimeImaging-impl/实时重建脚本/README.md`（MATLAB reference 与 production 关系说明）。
