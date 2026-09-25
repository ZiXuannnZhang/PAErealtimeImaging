# D5 重建算法文献补查

Task id: D5 — DAS 双波长质量增强 B 档任务集
Baseline: `origin/main` @ `65de7820ae8be7fb8f6910e6edaba18fad19acf0`
执行日期：2026-09-25
性质：纯文献调研，未修改任何代码

---

## 0. 核实等级纪律与检索边界（先说清楚，否则下面的等级没法判读）

沿用原报告纪律：每条结论标注 `已核实 / 间接核实 / 未完全核实`；
**标注「未完全核实」「间接核实」的不得作为设计依据**。

**本次检索的可达性限制（如实声明）**：

| 通道 | 状态 | 影响 |
|---|---|---|
| `api.crossref.org` | ✅ 可达 | 题录（题名/作者/卷期页/DOI）可**已核实** |
| `arxiv.org` / `export.arxiv.org` | ✅ 可达 | **摘要原文可读**，arXiv 全文（HTML/TeX）未逐一取 |
| 出版商全文（APS / SIAM / IOP / QIMS / PMC / ScienceDirect） | ❌ 不可达（连接被拒） | **没有任何一篇论文的正文被读到** |

⇒ 因此本任务中「已核实」最高只能到 **题录 + 摘要原文**；
凡涉及**正文公式编号、图、数值结果**的结论，一律标 `未完全核实`。
这本身就是一条重要交付物结论：**在当前环境下无法完成需要读正文的核验**，
需要可读全文的环境再补一次。

---

## D5-1 2D 环形/柱面反演的严格性边界（优先级最高）

### 1.1 该 2D/3D 差异的原始出处与准确表述

**找到的最直接来源（摘要原文已读）**：

> M. Haltmeier, *"Universal inversion formulas for recovering a function from
> spherical means"*, **SIAM J. Math. Anal. 46 (2014) 214–232**.
> DOI: [10.1137/120881270](https://doi.org/10.1137/120881270) ·
> arXiv: [1206.3424](https://arxiv.org/abs/1206.3424)
> 核实等级：**已核实（题录 + 摘要原文）**

摘要要点（原文已读，逐条对应）：

1. 给出 **arbitrary dimension** 的 universal back-projection 型重建公式，
   积分曲面是**任意光滑凸域的边界**，平均是**以边界上一点为心的球面平均**。
2. **前提：未知函数支撑在该凸域内部**（"Provided that the unknown function is
   supported inside that domain"）。
3. 一般凸域下，重建 = 真值 **＋ 一个显式可算的平滑积分算子**（不是精确反演）。
4. **椭圆域下该算子退化为零** ⇒ 在椭圆域边界上的球面平均有**精确反演公式**，
   且**对任意维数成立**。

**对前提 G1 的处理（这是本节最重要的产出）**：

前提 G1 写「3D 满足 Huygens 原理 ⇒ 局部反投影成立；2D 不满足 ⇒ 严格 2D 反演需
非局部（log 核）滤波（Finch–Haltmeier–Rakesh）」，并据此推「UBP 在本项目只能是近似」。

按 Haltmeier 2014 摘要，**这个表述需要收窄**：

- 「2D 与 3D 的反演公式不同形」「2D 相对 3D 多一个非局部算子」——方向上与摘要
  一致（一般凸域确实只到「＋平滑算子」），标 `间接核实`；
- 但**不能**由此推出「2D 下 UBP 只能是近似」。圆是椭圆的特例，按该文结论，
  **完整圆边界 + 源在环内**时，任意维数（含 2D）都有精确反演公式。
- ⇒ **G1 的正确收窄版本是「严格性不卡在 2D/3D，而卡在“源是否在探测面内”」。**

**这条收窄直接命中本项目**：本项目 `radius = 6.57 mm`、`fov = 36 mm`，
**FOV 大部分在探测环之外**。D3 实测该比例为 **89.54 %**。
即「源支撑在域内」这一前提在本项目**大部分像素上不成立**。
所以本项目**不能**作精确性声称——但**理由不是「2D 不严格」，而是「源在探测域外」**。

> 交付给文档的措辞修订建议：把「UBP 在 2D 只能是近似」改为
> 「UBP/反投影族的精确性结论以“源支撑在探测面内”为前提；本项目 FOV（36 mm）
> 大部分位于探测环（R=6.57 mm）之外，该前提不成立，故本项目不得作精确性声称。」

### 1.2 「单环平面 UBP 近似」相对严格反演具体损失什么

标 **`未完全核实`**。

本环境取不到任何正文，无法读到公式的具体形式、误差项的形态、或任何量化表。
仅从摘要可读出的一条结构性事实：一般凸域下多出来的项是
**一个显式可算的平滑积分算子**（smoothing integral operator）。
「平滑」一词在摘要中出现，因此**低频保留 / 高频衰减**这一类后果是**间接核实**的
方向性判断；幅值标度损失、伪影形态，**均未核实**，不得引用为设计依据。

### 1.3 本项目下 UBP 能声称到什么程度 —— 可写进文档的措辞边界

| | 措辞 | 依据 / 等级 |
|---|---|---|
| ✅ 能说 | 「反投影族（DAS / UBP）在本项目的几何下是**近似重建**」 | Haltmeier 2014 摘要（源在域内才精确）+ D3 实测 89.54 % 像素在环外。**已核实（摘要）+ 代码实测** |
| ✅ 能说 | 「UBP 与 DAS 的权重相差 `R/d`，属**两种不同算法**，不是同一算法的两种参数」 | 前提 E1，D3 数值独立复核（`dΩ/w = R/d`，最大相对偏差 4.6e-16）。**已核实（代码实测 + 推导）** |
| ✅ 能说 | 「B1 信号项 `2p − 2t·p′` 属于混合数据（mixed data）反演的形态」 | Dreier & Haltmeier 2022 摘要。**已核实（摘要）** |
| ⚠️ 有条件说 | 「在源完全位于环内的小 FOV 子域上，反投影族可能接近精确」 | Haltmeier 2014 摘要（椭圆域算子为零）；**本项目未在该子域上验证**。标 `间接核实` |
| ❌ 不能说 | 「UBP 输出是吸收能量密度、单极、非负」 | 前提 E2 已判为**事实错误**；Shen 等 J. Phys. D 54, 074001 系统给出负值伪影机理。**不得引用** |
| ❌ 不能说 | 「UBP 在 2D 解析精确」/「Huygens 原理保证本项目精确」 | 源在域外；且 2D Huygens 不成立。**不得引用** |
| ❌ 不能说 | 任何「文献量化了损失多少」的数值 | 全部 `未完全核实` |

---

## D5-2 双波长定量对重建侧的硬要求

### 2.1 重建侧必须保持的数学性质

| 性质 | 可操作判据 | 本项目落地 | 核实等级 |
|---|---|---|---|
| **线性** | 重建算子 `T` 满足 `T(a·p1 + b·p2) = a·T(p1) + b·T(p2)` | R3；B1 的 `inversionKernel` 是线性组合 | **已核实（推导 + D7 实测）** |
| **逐像素幅值比保持** | 同一线性算子作用于 wl1/wl2，输出比 = 输入比 | R1 判据；D4 比值守卫已实现 | **已核实（D4 18/18 + D7 8/8）** |
| **跨波长同标度** | 归一化除数必须与波长无关 | `accW` 只含几何量 ⇒ 两波长 `accW` 相同 | **已核实（代码实测 + 前提 G3）** |
| **不引入无记忆非线性** | 禁止逐图峰值归一化 / 整流 / Hilbert 取模 | R1 明令禁止 | **已核实（D4 负例判别力）** |

**判据（可写进验收）**：
> 对任意两个同几何的双波长输入 `p1 = k·p2`，重建后必须逐像素满足 `I1/I2 = k`
> （相对标准差 ≤ 容差）。任何使该断言失败的后处理都不得进入数据链。

这条在 D4 已实现为 `dualWavelengthRatioGuard`，并配了三个负例
（逐图峰值归一化 / 硬整流 / 单侧取模），**三个负例都确实使守卫失败**，判别力成立。

**文献支撑情况**：该判据本身是 R1 的直接推论，**不需要**文献背书即可作为验收判据
（它可自验）。若要引用文献，本次**未找到**给出「双波长比值 vs 重建算子」的直接文献，
如实记为：**未找到**。

### 2.2 fluence 校正与低频保留

**前提文档引用的是「Vu 等, IEEE TMI 43(2), 771 (2024)」。本次检索结果：**

> ❌ **未能核实**。在 Crossref 上按题名/作者/卷期页多路检索，**找不到**该条目。
> 最接近的是 Naser 等, *IEEE TMI* **38**, 561–571 (2019),
> DOI [10.1109/TMI.2018.2867602](https://doi.org/10.1109/TMI.2018.2867602)
> (*"Improved Photoacoustic-Based Oxygen Saturation Estimation With SNR-Regularized
> Local Fluence Correction"*)——但**作者、卷、页、年份都不同**，不是同一条。

**处理**：按核实纪律，**该引用不得作为设计依据**。请文档侧核对 DOI/卷期；
若确有此文，请给出 DOI 再补。

与 fluence / 低频相关的**已核实**旁证（题录）：

- Naser 等 TMI 38 (2019) 561：fluence 校正用于 sO₂ 估计 —— **题录已核实，正文未读**。
- Alles 等 IUS 2014 1284：无 fluence 估计的 sO₂ 成像 —— **题录已核实**。

因此「低频保留」这一条，本次只能给方向性判断（`间接核实`）：
**任何重建侧的高通类处理（含求导）都会衰减低频，而 sO₂ 比值对低频能量敏感**，
这与 D1 的导数可行性结论同向。**具体保留到多少 Hz，未核实。**

### 2.3 「重建算法选择对 sO₂ 误差的影响」有无文献

**明确：本次未找到。**

按题名/关键词检索（Crossref + arXiv）没有命中
「reconstruction algorithm choice → sO₂ error」的对比研究。
如实记录为：**未找到，不作设计依据**。

（提示：这恰好说明 D7 这类自建 A/B 有独立价值——文献层面缺这块对照。）

---

## D5-3 环形扫描 + 有限尺寸探头的反投影权重

### 3.1 B. Wang 等 QIMS 2019（虚拟探测器改善切向分辨率）

**题录（已核实）**：

> B. Wang, T. Su, W. Pang, N. Wei, *"Back-projection algorithm in generalized form
> for circular-scanning-based photoacoustic tomography with improved tangential
> resolution"*, **Quant. Imaging Med. Surg. 9(3), 491–502 (2019)**.
> DOI: [10.21037/qims.2019.03.12](https://doi.org/10.21037/qims.2019.03.12)

**方法细节与适用条件**：标 **`未完全核实`**。
该刊为开放获取，但本环境对该域连接被拒，**正文未读到**。
⇒ 其「广义形式」「虚拟探测器」的具体定义、适用条件、以及是否给出定量改善，
**均未核实**，不得写进设计依据。

**能说的**：题目自陈「circular-scanning + improved tangential resolution」，
与本项目几何（环形扫描）**直接相关**，应作为 B 系列后续的优先补读对象。

同类可替代线索（题录已核实）：

- J. Xiao, X. Luo, K. Peng 等, *"Improved back-projection method for
  circular-scanning-based photoacoustic tomography with improved tangential
  resolution"*, **Appl. Opt. 56, 8983 (2017)**,
  DOI [10.1364/AO.56.008983](https://doi.org/10.1364/AO.56.008983)

### 3.2 有限探头尺寸对反投影**权重**的影响

**最相关的文献（题录已核实）**：

> M. Haltmeier, G. Zangerl, *"Spatial resolution in photoacoustic tomography:
> effects of detector size and detector bandwidth"*, **Inverse Problems 26,
> 125002 (2010)**.
> DOI: [10.1088/0266-5611/26/12/125002](https://doi.org/10.1088/0266-5611/26/12/125002)

**结论**：该文**直接命中**本问（题名即 detector size / detector bandwidth 对空间
分辨率的影响）。但**正文未读**，故：

- 「有限探头尺寸影响的是**权重**而不仅是 PSF」—— **`未完全核实`**；
- 与 UBP 立体角权重的关系 —— **`未完全核实`**。

**本次可以给出的、不依赖正文的结构性判断（`已核实（推导）`）**：
共享前向模型 `RingPhantomForward` 的 `directivity` 项正是
`cosα = (R − r·n̂)/d`（`RingPhantomForward.cpp`，`RingPhantomForward.h` 注释），
与 UBP 立体角权重 `dΩ = R·Δθ·cosα/d²` 的 `cosα` **同源**。
⇒ 若做有限探头尺寸，最自然的落点就是把这个 `cosα` 因子**从可选开关变成必然项**，
并按探头孔径角给它一个有限的角向积分，而不是只改 PSF。这是**结构建议**，
不是文献结论。

### 3.3 前提 E1 的核验（Xu & Wang 2005 的权重精确表述）

**Xu & Wang 2005 题录（已核实）**：

> M. Xu, L. V. Wang, *"Universal back-projection algorithm for photoacoustic
> computed tomography"*, **Phys. Rev. E 71, 016706 (2005)**.
> DOI: [10.1103/PhysRevE.71.016706](https://doi.org/10.1103/PhysRevE.71.016706)

**原文 Eq.(20)–(22) 的精确表述（含 `dS₀`、`Ω₀`、法向约定、2D/3D 适用形式）**：
标 **`未完全核实`** —— 本环境取不到 APS 正文，**公式未读**。

**但是 E1 的推导本身已被两条独立证据核实，与是否读到原文无关：**

| 证据 | 内容 | 等级 |
|---|---|---|
| ① 数值复核 | D3 独立实现权重几何，随机采样 4000 组 (像素, 探测器)，验证 `dΩ/w = R/d`，**最大相对偏差 4.638e-16**（float64 舍入级） | **已核实（数值）** |
| ② 代码同源 | 共享 `RingReconInversion.h` 中 `dasWeight = wscale·dotp/(R·dsafe²)`、`ubpWeight = wscale·dotp/dsafe³`；代入 `wscale=−Δθ`、`dotp = r·r_s−R²`、`cosα=(R−r·n̂)/d` 展开得 `w = Δθ·cosα/d`、`dΩ = R·Δθ·cosα/d²`，二者之比 `R/d` | **已核实（推导 + 代码实测）** |

**结论：E1 的推导正确。**

> **D5-3 对 E1 的核验结论 = 「E1 推导正确」**，依据是上述两条；
> **但「Xu & Wang 2005 原文 Eq.(22) 的字面形式」仍未读到**，
> 若文档要逐字引用原文公式，需在可读全文的环境再核一次。

**附带发现（对原报告的另一处引用问题）**：
前提 §5 参考表第 4 条写「Finch, Patch, Rakesh, SIAM J. Math. Anal. 35, 1213 (2004)」。
按 Crossref，该文是 **David Finch, Sarah K. Patch** 两位作者，
*"Determining a Function from Its Mean Values Over a Family of Spheres"*,
**SIAM J. Math. Anal. 35, 1213–1240 (2004)**,
DOI [10.1137/S0036141002417814](https://doi.org/10.1137/S0036141002417814)。
作者列与前提文档不一致（多一位 Rakesh）。另有
Finch & Rakesh 的章节 *"Recovering a Function from Its Spherical Mean Values in
Two and Three Dimensions"*（2009/2017），
DOI [10.1201/9781420059922.pt3](https://doi.org/10.1201/9781420059922.pt3)——
**该标题正好是「two and three dimensions」**，是 G1 的候选出处。
标 `已核实（题录）`；内容未读。

---

## 引用清单

| # | 出处 | DOI | 核实等级 | 全文/摘要 |
|---|---|---|---|---|
| 1 | M. Haltmeier, *Universal inversion formulas for recovering a function from spherical means*, SIAM J. Math. Anal. **46**, 214–232 (2014) | [10.1137/120881270](https://doi.org/10.1137/120881270) · [arXiv:1206.3424](https://arxiv.org/abs/1206.3424) | **已核实** | 题录 + **摘要原文** |
| 2 | F. Dreier, M. Haltmeier, *Photoacoustic inversion formulas using mixed data on finite time intervals*, Inverse Problems (2022) | [10.1088/1361-6420/ac747b](https://doi.org/10.1088/1361-6420/ac747b) · [arXiv:2111.02262](https://arxiv.org/abs/2111.02262) | **已核实** | 题录 + **摘要原文** |
| 3 | M. Xu, L. V. Wang, *Universal back-projection algorithm for PACT*, Phys. Rev. E **71**, 016706 (2005) | [10.1103/PhysRevE.71.016706](https://doi.org/10.1103/PhysRevE.71.016706) | 题录已核实 | 仅题录（正文未读） |
| 4 | D. Finch, S. K. Patch, *Determining a Function from Its Mean Values Over a Family of Spheres*, SIAM J. Math. Anal. **35**, 1213–1240 (2004) | [10.1137/S0036141002417814](https://doi.org/10.1137/S0036141002417814) | 题录已核实 | 仅题录 |
| 5 | D. Finch, Rakesh, *Recovering a Function from Its Spherical Mean Values in Two and Three Dimensions* (2009/2017) | [10.1201/9781420059922.pt3](https://doi.org/10.1201/9781420059922.pt3) | 题录已核实 | 仅题录 |
| 6 | M. Haltmeier, G. Zangerl, *Spatial resolution in PAT: effects of detector size and detector bandwidth*, Inverse Problems **26**, 125002 (2010) | [10.1088/0266-5611/26/12/125002](https://doi.org/10.1088/0266-5611/26/12/125002) | 题录已核实 | 仅题录 |
| 7 | B. Wang, T. Su, W. Pang, N. Wei, *Back-projection algorithm in generalized form for circular-scanning-based PAT with improved tangential resolution*, Quant. Imaging Med. Surg. **9**, 491–502 (2019) | [10.21037/qims.2019.03.12](https://doi.org/10.21037/qims.2019.03.12) | 题录已核实 | 仅题录（开放获取，但本环境不可达） |
| 8 | K. Shen, S. Liu, T. Feng, J. Yuan, *Negativity artifacts in back-projection based PAT*, J. Phys. D **54**, 074001 (2020/21) | [10.1088/1361-6463/abc37d](https://doi.org/10.1088/1361-6463/abc37d) | 题录已核实 | 仅题录 |
| 9 | K. M. Kempski 等, *Application of the gCNR to assess photoacoustic image quality*, Biomed. Opt. Express **11**, 3684 (2020) | [10.1364/BOE.391026](https://doi.org/10.1364/BOE.391026) | **已核实（题录）** | 仅题录（D4 的 gCNR 口径来源） |
| 10 | M. A. Naser 等, *Improved PA-Based sO₂ Estimation With SNR-Regularized Local Fluence Correction*, IEEE TMI **38**, 561–571 (2019) | [10.1109/TMI.2018.2867602](https://doi.org/10.1109/TMI.2018.2867602) | 题录已核实 | 仅题录 |
| 11 | J. Xiao, X. Luo, K. Peng 等, *Improved back-projection method for circular-scanning-based PAT with improved tangential resolution*, Appl. Opt. **56**, 8983 (2017) | [10.1364/AO.56.008983](https://doi.org/10.1364/AO.56.008983) | 题录已核实 | 仅题录 |
| — | 「Vu 等, IEEE TMI 43(2), 771 (2024)」 | — | **未能核实（检索无果）** | — |

---

## D5-1 措辞边界清单（可直接写进项目文档）

**能说：**

1. 本项目采用反投影族（DAS / UBP），在本几何下是**近似重建**，不得作解析精确声称。
2. 精确性结论以「**源支撑在探测面内**」为前提；本项目 FOV=36 mm、探测环 R=6.57 mm，
   **89.54 % 的 FOV 像素在环外**（D3 实测）⇒ 该前提大面积不成立。
3. UBP 与 DAS 是**两种不同算法**：权重相差 `R/d`（随像素变化），
   信号项相差 `2p−2t·p′` vs `p`；**R2 要求二者成对切换**。
4. B1 的信号项 `2p−2t·p′` 属**混合数据（mixed data）**反演形态；
   截断采样（`sampDepth=4000`）对应**有限时间区间**，两者都已有对应理论文献。

**不能说：**

5. ❌「UBP 输出单极、非负、等于吸收能量密度」（前提 E2 已判为事实错误）。
6. ❌「2D 下反投影必然只是近似 / 2D 不严格」（不准确；真正前提是源在域内）。
7. ❌ 任何「文献量化了近似损失」的数值（本次全部未核实）。
8. ❌ 引用「Vu 等 TMI 2024」作为低频保留依据（检索无果）。
9. ❌ 引用 Xu & Wang 2005 的**具体公式编号与字面形式**（正文未读）。

---

## 未验证项 / 限制

- **本环境取不到任何出版商正文**；所有涉及公式、图、数值的结论均为 `未完全核实`。
- 未能核实前提文档引用的「Vu 等, IEEE TMI 43(2), 771 (2024)」。
- D5-1 的「损失什么」（幅值/分辨率/伪影形态）完全未量化。
- D5-3.1 的 Wang QIMS 2019 方法细节未读（该刊开放获取，换个网络环境应可补）。
- D5-3.2 有限探头尺寸对**权重**的影响未核实（仅给了结构建议）。
- 本次未对算法在本项目数据上的效果做任何预估（那属 D7）。
- 证据口径：`source/code correctness` 未声称；`automated tests` 不适用；
  `real hardware validation` 不适用（纯文献）；`hardware root-cause attribution` 未声称。
