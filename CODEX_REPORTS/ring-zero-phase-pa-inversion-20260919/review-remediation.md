# 阶段 A 审查整改回执（R1–R5）

任务：`TASKS/环形成像阶段A审查整改追加_20260919-201935.md`（REQUEST_CHANGES）
分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
starting SHA：`94939627f48ccc95f60c4995c99be4e371aaa2ca`（= 任务发布时远端 HEAD，已核对）
final SHA：见提交（本文件所在提交）；local HEAD == remote HEAD 经 `git ls-remote` 核对（回执见文末）。
环境：MATLAB R2023a（9.14.0.2206163）+ Signal Processing Toolbox，Windows；未使用 SciPy。
提交方式：全部整改为**新增提交**（未 amend/rebase/squash/force push），未合并 main，未开始 B/C，未修改生产代码。

统一复跑入口：`matlab -batch "run_all_remediation"`（工作目录 = `CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/matlab/`），本轮最终运行 **8/8 项 OK、汇总 0 项失败、进程退出码 0**，总耗时 ~65 s（各项耗时见文末回执）。

---

## R1（P1）exp2 时间导数维度错误 —— 已修复 + 自动测试

**缺陷确认**：9493962 `matlab/exp2_ring_ubp_2d.m:42,71` 的 `ppdA = gradient(double(pAd), dr/c, 1)`——MATLAB R2023a 中该调用的第三参数是 dim2（列/A-line）间距（`gradient(F,hx,hy)` 形式），且单输出返回 dim2 方向梯度。本机独立复现：`A = repmat((0:4)',1,3); gradient(A,0.1,1)` 返回全零（正确时间导数应为 10）；`[~,Gt] = gradient(A,1,0.1)` 第二输出才沿行方向返回 10。与审查描述一致。

**修复位置**：
- 新增 `matlab/timeDerivative.m`——dim 1（时间）逐列独立导数唯一入口：内部中心差分 `(F(i+1)−F(i−1))/(2dt)`、端点单侧、dt 显式秒单位；对矩阵直接调用 `gradient` 的用法在本目录内已全部清除（exp2/exp3/exp5 均改用 timeDerivative；exp1/exp6 的 `gradient(vector,·)` 为向量用法，非矩阵误用，按任务要求未做盲目替换）。
- `matlab/exp2_ring_ubp_2d.m`：`ppdA`/`ppdB` 改用 `timeDerivative(pAd, 1/fs)`。
- `matlab/exp3_time_axis.m`：T1–T4 的导数改用 `timeDerivative(cut, 1/fs)`（向量路径，与原向量 gradient 语义一致 ≤eps）。
- `matlab/exp5_order_endpoints.m`：b 路导数全部 `timeDerivative`。

**自动测试**（`matlab/test_time_derivative.m` → `evidence/test_time_derivative.json`，任务 §7.1 四项全覆盖）：

| 项 | 断言 | 结果 |
|---|---|---|
| T1 | 时间线性、各列相同 → 导数=已知常数 a（maxErr 1.86e-7，容差 3.7）；诊断字段 `T1_legacyExprMax=0` 记录旧写法在同输入返回全零——本测试能明确识别原错误 | PASS |
| T2 | 时间恒定、仅列间变化 → 时间导数逐元素精确 0 | PASS |
| T3 | 多列不同振幅/频率解析信号 vs 解析导数，容差=(ω·dt)²/6（内部）与 ω·dt/2（端点）：实测 0.00263 ≤ 0.00263、0.0547 ≤ 0.0628 | PASS |
| T4 | 向量路径（timeDerivative 列 == gradient(v,dt)，相对差 1.81e-14）与矩阵路径（== [~,gradient(F,1,dt)]，1.81e-14）一致；端点单侧差分整数精确例 [0;1;4;9]→[1;2;4;5] | PASS |
| T5 | 导出 p′/b 参考向量未受矩阵误用影响：重算相对差 3.15e-13 / 3.33e-13（≤1e-12） | PASS |
| 输入校验 | dt 非法/时间维过短报错 | PASS |

**结论变化**：exp2 全部指标重算（见 R2/R5）；exp3 T2 的 p′/b 对齐结论不变（−0.140/+0.010/+0.003 样本）；exp6 导出向量与阶段 B 容差建议不变。

---

## R2（P1）旧权重对照被自归一化成符号图 —— 已修复 + 自动测试

**缺陷确认**：9493962 `reconUBPwrong` 仅返回 acc，单参数 `normAcc` 执行 `acc./max(abs(acc),1e-12)`。旧证据 `exp2_ring_ubp_2d.json` 中 discWrong cv=0（常数符号）、四个对照目标 amp 全部 =1.000——确为符号图，其"位置误差 2.0–2.8mm、FWHM ~4mm"不可归因于权重。

**修复位置**（`matlab/reconPairKernels.m` 新增共享核 + `matlab/exp2_ring_ubp_2d.m` 重写）：
- legacy 对照改名 **legacyW**（中性命名），与 DAS 相同的显式两参数归一化 `normAccW(acc, accW)`，accW 用同一 `abs(w)` 累积规则；单参数 normAcc 已删除，`normAccW` 缺 accW 直接报错（防静默退化）。
- 五模式因子分解（固定输入 b/p、几何、插值、maskOob、角度采样、标定与测量方法，每次只改一个因素）：das（p+q1 权重+accW）、legacyW（b+q1+accW）、ubpD（b+立体角+accW）、ubpP（b+立体角+原始和式+C_P）、saP（p+立体角+accW，补格）。
- **权重族标签勘误（关联新发现）**：生产默认权重 = `Δθ·(−dotp)/(R·d²)`（CUDA `wExponent=1 → pw=2`；MATLAB 参考 `pw=p_exp+1` 同式）= Δθ·cosα/d。旧 exp2 的 "DAS q=1" 实为 `Δθ·(−dotp)/(R·d)` = Δθ·cosα（对应 wExponent=0，非生产默认）；旧 "wrong" 反而是生产默认权重。整改后 das/legacyW 均按生产默认实现。
- 指标整改：FWHM 沿目标相对环心**实际径向/切向方向**采样（`sampleAlong` 双线性插值），无半高交点/峰在窗边界标记不可用（本轮全部 ok）；环内/环外目标分列；新增导数主导性实测（§R5）。

**自动测试**（`matlab/test_fair_comparison.m` → `evidence/test_fair_comparison.json`）：

| 项 | 断言 | 结果 |
|---|---|---|
| U1 | normAccW 已知值精确 [0.5,−0.6]；缺 accW 报错 | PASS |
| U2 | das 与 legacyW 的 accW 逐元素一致（同一 abs(w) 规则），相对差 0 | PASS |
| U3 | ubpD 与 saP 的 accW 逐元素一致，相对差 0 | PASS |
| U4 | 对照输出不退化为 sign(acc)：legacyW 峰值/背景 5.4、ubpD 3.8（≥3）；旧符号图同幻体该比值 = 1.000（证明本测试可识别原缺陷） | PASS |
| U5 | legacyW vs ubpD 成像形状相关（同一 b 输入）ROI 内 Pearson = 0.939（≥0.6） | PASS |

**重跑后的结论变化**（均匀盘 CV / 点目标，全部重新计算，不继承旧报告优劣）：
- 盘内 CV：das 0.0153 / legacyW 0.0334 / **ubpD 0.0080** / ubpP 0.1084 / saP 0.0874（旧：das 0.0901 / ubpD 0.0874 / ubpP 0.1909 / wrong 0——旧 das 基数因权重标签错误而无效）。
- 公平 legacyW 对照环内位置误差 0–0.2mm、FWHM 与 ubpD 相当——**旧"错误配对位置误差 2.0–2.8mm、FWHM 恶化 3×"结论撤回**。
- 优劣互现（新）：ubpD 盘内平坦度最优、近场伪影较低（0.616 vs 1.087）；legacyW 点目标 CNR 更高（15.6 vs 9.2）、环外位置误差更小（0.283 vs 1.131mm）。**"反演开启即切换立体角权重+accW"降级为待审建议，作为决策点返回**（algorithm-stage-A.md §4.3）。

---

## R3（P1）delayCut=0 完整反演证据缺失 —— 已补齐（两态完整反演 + 查询测试）

**生产语义核对**（9493962 源码，非"已证实生产 bug"）：`preprocessBlock` delayCut=0 时 srcRow0=0 全行保留；`ring_recon_cuda.cu` 全文无 systemDelay 引用，查询 tf=d·fs/c 直接作用于存储行 → **生产 delayCut=0 旧行为把原始样本 1 当声学零点，不做延时补偿**，与拟议反演语义 t=(τ+1−sysDelay)/fs 不等价。

**新增内容**（`matlab/exp3_time_axis.m` T5/T6，证据 `evidence/exp3_time_axis.json`）：

- **T5 两态完整反演等价（均匀声速）**：同一校准声学信号 pAc(t)（双高斯脉冲，解析+解析导数），经 sysDelay ∈ {358(wl1), 371(wl2), 359(通道差)} 写入 raw；delayCut=1（裁剪线，t=τ/fs）与 delayCut=0（全行，t=(τ+1−sysDelay)/fs）在同一物理走时处比较 p/p′/b。查询 τ = [700, 703.64, 671.14, 1090.60, 2400] 样本（小数部分 0/0.64/0.14/0.60/0——**整数与亚样本覆盖**）。
  - 两态差：max|Δp| = **9.77e-15**，max|Δb| = **3.13e-11**（同一插值误差下的浮点级；断言容差 1e-12 / 1e-9·|b|max）→ PASS
  - 跨通道差（同物理走时，wl1/wl2/chVar）：p、b 均为 **0** → PASS
  - ±1 样本扰动：固定走时处 max|Δb| = **255.6**（≥0.1·|b|max=0.164）→ 可分辨 PASS
  - 不补偿延时（生产旧行为）：max|Δp| = **0.985**（≈信号错位 sysDelay/fs）→ 与拟议语义的差距量化；阶段 B 需局部适配（tf_eff = tf + sysDelay − 1 或反演强制 delayCut=1），本阶段未改生产代码。
  - 解析参考：p 误差 0.0177（≤线性插值界 0.04，判定项 PASS）；b 误差 2.86（相对 |b|max 1.74）**仅报告不判定**——这是 σ=2 样本应力脉冲在亚样本 τ 处 p′ 线性插值误差被 2t 放大的固有特性（两态共享同一插值误差，不影响两态等价结论；exp2 体模脉宽 ~84 样本下可忽略；JSON `analyticBNote` 字段注明）。
- **T6 双声速路径**：分层走时（τL=1088.3487 样本，像素原点、c1/c2=1490/1540、L1=4.0000mm）处两态 p/p′/b 差 = **0**；同速退化 |Δτ| = 0；解析 p 误差 0.0278（≤0.04）→ PASS。
- 完整查询链表格化字段（JSON T5 各通道 p/b 数组 + 查询 τ/小数部分 + 参数说明）：原始索引、裁剪索引、校准声学时间、像素物理走时、实际取样位置与 b 计算的对应关系在 algorithm-stage-A.md §2.2 表中固化。

**结论变化**：原 T1–T4 结论维持（p′/b 对齐 +0.010/+0.003 样本；±1 可分辨 2.0 样本）；新增"两态在同一物理走时取到同一信号"的直接证据（不再是重标样本时间）；新增生产旧行为差距量化与阶段 B 适配要求（algorithm-stage-A.md §2.3/§7）。

---

## R4（P1）DBR/滤波边界策略未冻结 —— 已冻结（拟定默认策略 + 非默认配置证据）

**整改内容**（`matlab/exp5_order_endpoints.m` 重写，证据 `evidence/exp5_order_endpoints.json/.mat`）：

- **同一实现对照（R5 归因修复）**：顺序 N（提议：DBR→削顶→DelayCut→HP→LP 整条裁剪线）与顺序 M（历史切片 301:/sysDelay+20:）全部用 refZeroPhase；历史 filtfilt 实现作为 Mff 第三行单列。
  - 顺序效应（同实现）：echoOnly 远区 **0**；burstEcho 远区 **0.0133**（切片边界切进 burst 暂态所致）。
  - 实现效应（同顺序）：**1.61e-9 / 2.61e-9**——旧版把顺序与实现混合归因的问题已分离。
- **干扰/目标分离**：echoOnly（无干扰参考）/ burstOnly（启动干扰残留，含逐 100 样本残留衰减曲线）/ burstEcho（干扰下目标）三组分别测量。
  - burstOnly：残留拖尾（>1%·burst 峰）N=1723/M=1722 样本；残留降至回波幅度 0.5 以下在 τ≈**3200**（合成 burst 2000 幅、1.5µs 衰减）。
  - burstEcho 起始 100 样本 |p′|max：N=2.407e10 / M=2.505e10（burst 微分的物理量级）。
- **DBR 末端矩阵**（zeroRows ∈ {300,313,358,400,413}，覆盖 早于/等于/晚于 裁剪起点 与 dbrmaskExtra ∈ {0,13,100}）：

| zeroRows | vs 裁剪起点 | 置零段输出\|max\| | 阶跃差 max | 污染降至回波 1% | 目标 τ=800 幅值比/位偏 | memHP 裕量覆盖 |
|---|---|---|---|---|---|---|
| 300/313 | 早于 | 0 | 0 | — | 1.0000/+0.000 | — |
| 358 | 等于 | 396 | 0* | — | 1.0000/+0.000 | — |
| 400 | 晚于 | 562 | 2074 | τ=2737 | 1.0356/−0.372 | **否**（2327<2737） |
| 413 | 晚于 | 296 | 2032 | τ=2734 | 1.0293/−0.408 | **否**（2327<2734） |

  *358 行截断点恰为 burst 起点 t=0（sin0=0），无实际阶跃。置零段输出非零（296–562）本身即"双向滤波非因果回卷污染置零段"的直接证据。
  - **memHP 固定裕量也不足以覆盖实测污染范围**（dbrEnd+1914=2327 < 2737），且污染长度依赖被截断信号的幅度/长度（数据相关）→ 固定裕量方案被证据否定。
- **近边界目标（B6，边界约定误差 vs 干扰效应分离）**：与"裁剪起点前移 pad=2000 的同声学场景理想参考"比（边界约定误差 BC），与 echoOnly 比（干扰效应 INT）：
  - **BC：τ = 400/600/800/1200/2700 全部 ampRatio = 1.000000、posBias = +0.0000（p 路与 b 路）**——稳态起点线的边界约定误差精确为 0，包括位于 848–3639"记忆区间"内的 τ=400/600/800 → **否定"机械丢弃 848–3639 样本"**。
  - INT（burst 干扰效应，数据相关）：τ=400 p 路 4356（被淹没）/ b 路 549；τ=2700 p 路 9.25 / b 路 3.19——本合成 burst 下可检测性受限至 ~3200 样本；实机适用范围 UNVERIFIED。
- **其余覆盖**：滤波组合 none/HP/LP/HP+LP × 反演 p/b 路（B4：带内目标幅值 0.489900 不受滤波影响，b 路头部残差与 p 路同源）；窗长 Nt ∈ {2000,4000,8000}（B5：幅值比 1.000000）；wl2 sysDelay=371（B7：1.000000）；短窗报错 12 报错/13 正常（A5，维持）。

**冻结的拟定默认策略**（algorithm-stage-A.md §3.3，待审查）：

1. 顺序：DBR置零(1..maskLength+extra) → 削顶 → DelayCut → HP(整条裁剪线) → LP(整条裁剪线) → [导数 p′ → b]。
2. 端点：refZeroPhase 稳态初始化 + 奇对称延拓 nfact=3n；线长≤3n 或非有限输入报错。
3. 有效查询区：原始行坐标 m ≥ max(sysDelay, dbrEnd+1)，默认不加裕量。
4. **滤波（含反演）启用时要求 dbrEnd = maskLength+dbrmaskExtra < sysDelay，否则报错拒绝配置**（默认 313 < 358/371 满足；dbrmaskExtra 增大可触发）——依据 B2：阶跃入线时污染传播 ~2737 样本、超出 memHP 裕量且数据相关。
5. 头部不加固定裕量、不替换前段；启动干扰按残留曲线/干扰效应量化报告，不做固定丢弃。
6. 端点导数：timeDerivative（中心差分/端点单侧、逐列），与滤波同一裁剪线。
7. 明确不采纳：848–3639 全局丢弃。

---

## R5（P2）证据表述及规格误导 —— 已逐项修订

| 项 | 修订 | 位置 |
|---|---|---|
| exp1 球外残差"确认为求积伪影"过强 | 措辞收敛为"与数值离散误差解释一致，机制尚未隔离"，不以下更强结论；球内 6.4e-8 判定不受影响；收敛序列 −0.358→−0.098→−0.051 如实保留 | algorithm-stage-A.md §1.2；exp1 未改码重跑复核（收敛序列 −0.35845/−0.09819/−0.05090） |
| §4.6"实测导数项大 2–3 量级" | 以修正导数重新实测：查询处 RMS\|2t·p′\|/RMS\|2p\| = 盘 2.04、点目标 13.35/13.90/13.62/13.37（exp2 `derivDominance` 字段）——主导成立但约一个量级；旧实测主张撤回 | algorithm-stage-A.md §4.6 |
| exp5 顺序归因混淆 | 同 refZeroPhase 实现对照（顺序效应 0/0.0133）+ filtfilt 实现效应单列（1.61e-9/2.61e-9） | algorithm-stage-A.md §3.1；exp5 B1 |
| README "任务规格未推送"注记过时 | 更正为"远端已推送，提交 7236c03a69b2144e7307cd5135fe441fa6fd8cc4"（本机 `git show origin/codex/task-docs` 可见原任务文档） | README.md 头部 |
| 状态表述 | 全部改为"阶段 A 整改后待审查，B/C 未开始，整项功能未完成"；不再使用"生产冻结"措辞（改为"拟定/建议/待审"） | README.md、algorithm-stage-A.md 全文 |

---

## 原推荐保留 / 撤销 / 返回决策点

| 原 9493962 推荐 | 状态 |
|---|---|
| 反演公式 b = 2p − 2t̃·∂p/∂t̃ + 立体角权重（Xu-Wang Eq.(20)–(22)），单环平面 UBP 近似定位 | **保留**（exp1 复核 6.4e-8；公式本身不受 R1/R2 影响） |
| 时间轴 t=τ/fs（delayCut=1）、不加回 sysDelay | **保留**（T1–T4 维持；新增两态等价与生产 delayCut=0 适配要求） |
| 滤波规格（double SOS、nfact=3n、稳态+奇延拓、复合增益 g²、n≥5 tf 病态） | **保留**（exp6 复核） |
| 滤波顺序：整条裁剪线（vs 历史切片） | **保留**，归因证据更换（同实现对照） |
| accW 归一化推荐 | **修订为待审**：ubpD 盘内 CV 最优（0.0080）但点目标 CNR/环外位置不如 legacyW——决策点返回 |
| "错误配对（wrong）实质性劣化（2–2.8mm）" | **撤销**（符号图伪影；公平对照下 0–0.2mm） |
| "导数项大 2–3 量级" | **撤销**，改为实测 2.0/13.4–13.9 |
| "起点裕量默认 0、建议后续配置" | **升级为确定策略**：默认 0 + dbrEnd<sysDelay 前置校验（B2 证据） |
| "848–3639 纳入有效区间定义（建议阶段 B 配置裕量）" | **撤销**该方向，明确不采纳全局丢弃；边界约定误差实测 0 |
| 距离权重标签（旧 exp2 的"DAS q=1"） | **勘误**：旧对照实为 wExponent=0 权重；生产默认 = Δθ·cosα/d |

## 两态查询公式（冻结约定，R3）

```
声学时间定义：t(m) = (m − sysDelay)/fs（m 为原始 1 基样本；sysDelay 每波长/通道独立）
delayCut=1：查询 τ = d·fs/c(+分层)（裁剪线连续坐标）          → t = τ/fs
delayCut=0：查询 τ₀ = d·fs/c(+分层)（全行连续坐标）           → t = (τ₀ + 1 − sysDelay)/fs
两态对应同一原始取样位置：连续原始坐标均为 τ + sysDelay（整数 sysDelay 下插值分数相同）
生产 delayCut=0 旧行为（无补偿）≠ 拟议语义 → 阶段 B 局部适配：tf_eff = tf + sysDelay − 1 或强制 delayCut=1
```

## 选定边界策略（拟定默认，待审查）

见本文件 R4 节第 2 段 7 条与 algorithm-stage-A.md §3.3 表。核心：不加固定头部裕量；dbrEnd≥sysDelay 报错拒绝；端点 = 稳态+奇延拓 + 短窗/非有限报错；干扰数据相关量化报告。

## 未解决决策点（返回规划主代理）

1. **权重/归一化最终选择**（R2）：维持"反演开启 → 立体角权重 + accW"（ubpD，盘内最优、物理依据明确）还是改选/允许配置 legacyW（点目标 CNR 与环外位置更优）——优劣互现，见 §R2 数据。
2. **delayCut=0 + 反演的适配方式**（R3）：查询补偿（tf_eff = tf + sysDelay − 1）还是强制 delayCut=1；对用户可见行为（报错/自动切换）的选择。
3. **环外像素策略**（维持原三项选项 a/b/c，本阶段推荐 a+c）。
4. exp1 球外残差机制如需更强结论，是否要求补充独立网格加密证据（本阶段未做，措辞已收敛）。

## 未验证项

- 真实采集数据（14.dat 链路）、真实噪声、真实启动 ring-down 参数——B1/B6 干扰量化为合成参数下结论，实机适用范围 UNVERIFIED。
- 反演相对 DAS 的质量收益（留待阶段 C 同数据四态对比 + 实机）。
- 性能（阶段 C）。
- 生产 delayCut=0 旧行为适配后的端到端行为（阶段 B 实现并测试；本阶段未改生产代码）。
- 本轮无生产实现改动，不声称生产算法回归。

## 阶段 B/C

**仍未开始。** 本次整改仅触及 `CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/` 内参考/测试/证据/文档；是否 APPROVE 由规划主代理独立复审决定。

## 复跑命令回执（最终记录）

工作目录：`CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/matlab/`；MATLAB R2023a 9.14.0.2206163。

```
命令：matlab -batch "run_all_remediation"
退出码：0；汇总：0 项失败；[RUN_ALL] 全部通过

[RUN_ALL] test_time_derivative.m (R1 导数自动测试)              OK  0.2s
[RUN_ALL] test_fair_comparison.m (R2 公平对照自动测试)          OK  0.2s
[RUN_ALL] exp1_ubp_sphere.m (exp1 UBP 常数（未改码，重跑复核）)  OK  0.1s
[RUN_ALL] exp2_ring_ubp_2d.m (exp2 修复后完整重跑)              OK 51.9s
[RUN_ALL] exp3_time_axis.m (exp3 时间轴 + 两态反演（重跑）)      OK  0.1s
[RUN_ALL] exp4_dual_layer.m (exp4 分层走时（未改码，重跑复核）)  OK  0.6s
[RUN_ALL] exp5_order_endpoints.m (exp5 顺序/边界策略（重跑）)    OK  0.2s
[RUN_ALL] exp6_filter_reference.m (exp6 滤波参考（未改码，重跑复核）) OK 0.2s
```

单项运行示例（均退出码 0）：`matlab -batch "run('test_time_derivative.m')"`、`matlab -batch "run('exp2_ring_ubp_2d.m')"` 等。exp1/exp4/exp6 代码未改动、重跑后 JSON 字节不变（确定性复核）；`filter_reference_vectors.mat` 因重跑重写（内容约定不变，T5 核查相对差 ≤3.3e-13）。

**证据体积说明**：`exp2_images.mat` 首版曾包含前向/导数大矩阵（~92MB，触发 GitHub 大文件警告）；已改为仅存储重建图像与指标（6.3MB），前向/导数可由 exp2 脚本确定性重算，标量结果（derivDominance 等）在 JSON 中不变。瘦身后全套件再次复跑：退出码 0、0 项失败（exp2 50.3s），本回执引用的全部数值不受影响。

主要结论 → 脚本/断言/数据字段映射：

| 结论 | 脚本 | 断言/字段 |
|---|---|---|
| 导数四项断言 + 导出向量未受影响 | test_time_derivative.m | res.pass / T1_legacyExprMax / T5_exportDpRel |
| legacyW 非符号图、权重族一致 | test_fair_comparison.m | U4_contrastLegacy / U2_accWMaxRel / U5_corrLegacyUbp |
| 盘 CV 五模式 | exp2 | discDas/discLegacyW/discUbpD/discUbpP/discSaP .cv |
| 点指标（方向 FWHM、环内外） | exp2 | points[].fwhmRadValid/fwhmRadReason/inRing |
| 导数主导性 | exp2 | derivDominance.disc/point1..4 |
| 两态等价/跨通道/±1/不补偿差距 | exp3 T5 | maxStateDiffP/B、maxCrossChannelDiffP/B、perturbMaxDb、maxNoCompensationDiff |
| 分层两态等价/同速退化 | exp3 T6 | maxStateDiff、degenErrSamples |
| 顺序/实现归因 | exp5 B1 | farRelDiff_N_vs_M / farRelDiff_M_vs_Mff |
| 边界约定误差 = 0 | exp5 B6 | bcRatioP/B、bcBiasP/B |
| DBR 阶跃传播与拒绝策略 | exp5 B2 | stepDiffBelowEcho1pctAt、memHPSpanCovers |
| 干扰残留曲线 | exp5 B1 | burstOnly.residueProfile |

## 远端核对回执

提交后执行（结果附于执行报告）：

```
git push origin codex/ring-zero-phase-pa-inversion-20260919-025754
git ls-remote origin codex/ring-zero-phase-pa-inversion-20260919-025754
```

核对项：local HEAD == remote HEAD（完整 SHA 见执行报告）。
