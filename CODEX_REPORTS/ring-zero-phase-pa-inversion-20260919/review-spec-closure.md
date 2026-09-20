# 阶段 A 二次整改收口回执：边界参考与时间轴规格（B1/B2/B3）

任务：`codex/task-docs:TASKS/环形成像阶段A边界参考与时间轴规格收口_20260919-235753.md`（REQUEST_CHANGES，第二次审查整改追加；任务发布 SHA `fe6735ac7924a42d6b4894b1b2d62b3d3204f057`）
分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
starting SHA：`9ace9e10f0886512954dac6f69ce01e715a4591e`（= 任务发布时远端 HEAD，已 `git ls-remote` 核对）
final SHA：见提交（本文件所在提交）；推送后 local HEAD == remote HEAD == ls-remote 三处核对结果附于执行报告。
环境：MATLAB R2023a（9.14.0.2206163）+ Signal Processing Toolbox，Windows。
提交方式：新增提交（未 amend/rebase/squash/force push），未合并 main，未开始 B/C，未修改生产代码。
上位任务与一轮整改回执：`环形成像零相位滤波与光声反演滤波_20260919-024359.md`、`环形成像阶段A审查整改追加_20260919-201935.md`、[review-remediation.md](review-remediation.md)（R1–R5）。

统一复跑入口：`matlab -batch "run_all_remediation"`（工作目录 = `matlab/`），本轮最终运行 **9/9 项 OK、汇总 0 项失败、进程退出码 0**，总耗时 ~91 s（含 MATLAB 启动；exp2 76.1 s 为大头）。

---

## B1（P1）B6 边界参考在滤波前退化为同一输入 —— 已重写 + 结构/负例/收敛测试

**缺陷确认**：9ace9e1 `exp5_order_endpoints.m:230–249` 的"理想参考" `rawI = makeRaw('echoOnly', tc, sysDelay+pad, Nt+pad)` 经 `processN`（`:372` `cut = x(sysD:end)`）先把增加的前段裁掉再滤波——rawE/rawI 送入滤波器的数组长度（同为 3643）、内容、边界完全相同。规划侧独立复现 `B6_filter_input_identical=1, maxdiff=0`；旧 G1_boundaryExact"边界约定误差精确 0"是自比伪影，不能支持任何"边界零误差/记忆区内无损恢复"结论。

**修复位置**：

- **`matlab/longReference.m`【新】**——独立长窗参考唯一入口：同一声学信号 `s(τ)` 在扩展区间 `[τ=−padL, Nt−sysD+padR]` 上先整条零相位滤波（同一 refZeroPhase SOS 实现）+ 求导（timeDerivative），再按物理时间提取 `τ∈[0, Nt−sysD]`。要点：额外段在滤波时真实存在（与旧"先平移 sysDelay 再裁剪"的本质区别）；声学原点 m=sysD 不随 pad 改变（不重新归零）；b 的时间乘子与短窗共用同一物理 t=τ/fs；DBR 置零按同一 raw 坐标规则（m∈[1,dbrEnd]）施加；独立性只来自"真实保留的不同边界"，复用同一 SOS 实现以隔离边界效应（任务 §7.1 允许）。
- **`matlab/ringAcousticModel.m`【新】**——exp5 原局部 makeRaw 的信号模型原样抽出共享，保证短窗/长窗"同一物理声学信号"逐位一致。
- **`matlab/exp5_order_endpoints.m` B6 重写**（B1/B2/B4/B5/B7/A5 块未动）：结构断言、旧构造负例、pad 收敛、BC/INT 主表、滤波组合覆盖、wl2 覆盖、起端/终端敏感性。
- **`matlab/test_boundary_reference.m`【新】**——结构/回归测试（S1–S6，见下），纳入 `run_all_remediation.m` 统一入口，失败非零退出。
- 断言门更换：`G1_boundaryExact`（自比恒等断言）**删除**；新增 `G1_refIndependent`（结构）、`G1b_refConverged`（pad 收敛）；`G3_defaultIdentical`（默认配置与自身参考恒等的同源自比断言）重定义为 `G3_dbrBeforeCropNoop`（zeroRows=313 vs 0 逐位一致，见 B2 附带说明）。

**证据**（`evidence/exp5_order_endpoints.json` B6 字段 + `evidence/test_boundary_reference.json`）：

1. **真实滤波输入范围（B6.reference / 测试 S1）**：短窗滤波输入 3643 样本（τ∈[0,3642]，生产 delayCut=1 语义）；长参考输入 19643 样本（τ∈[−8000,11642]），多出 16000 = 2×padRef 且在滤波时存在；长输入包含短窗输入为连续子段**逐位一致**；两路声学原点同为 m=358。有效查询区（默认 dbrEnd=313<sysDelay）：τ∈[0,3642]（B6.validQueryTauRange）。
2. **旧自比构造负例（B6.reference.oldConstructionMaxDiff / 测试 S2）**：对 pad=2000 与 8000 重建旧"先平移 sysDelay 再裁剪"构造，其滤波输入与短窗**逐位相同（maxdiff=0）**→ 被自动认定为无效独立参考；该构造一旦复活，S2 断言语义即失败。
3. **pad 收敛（B6.reference.convergence / 测试 S3，容差 1e-9 相对窗内幅度，记录值）**：目标 τ=800 窗 [300,1300]，对 padList=[250,500,1000,2000,4000,8000] 相对 pad=8000 参考的 max|Δ|rel——echoOnly 收敛于 pad 250（~1e-17 量级即已收敛）、burstOnly/burstEcho 收敛于 pad 500（250→2.3e-9、500→7.2e-10，曲线与 HP 记忆长度 memHP=1914 的尾部衰减一致）；三场景均在容差内收敛（门 G1b）。
   > **三轮 C1 注记（20260920）**：本条"收敛"当时由旧判据（convergedPad≤padRef，padRef 自比恒零计入候选）判定；三轮收口已把判据更换为 padConvergenceCheck（padRef 不作候选、≥2 非参考 pad、两两稳定、无再次超差；见 review-final-closure.md C1），真实曲线按新判据重算仍 PASS（echoOnly firstOkPad=250/nOk=5、burstOnly/burstEcho firstOkPad=500），上列各 pad 相对误差数值不变。
4. **同物理时间对齐（测试 S4）**：远场回波（τ=2700，Nt=8000，距两端 >HP 记忆）短窗 vs 收敛参考 ampRatio=1.000000000000、posBias=0、|Δp|rel=7.7e-14、|Δp′|rel=1.15e-14、|Δb|rel=1.14e-14——无整体平移、导数时间原点正确。错位提取负例（S2b，漏加 padL 偏移）：窗内幅值 4.35e-19 vs 正确 0.49（<1e-6 比值，可发现）；导数时间重新归零负例（S2c）：b 最大差 501.3（≥0.1·|b|max=10.03，可测）。
5. **BC 主表（B6.rows，echoOnly vs 收敛参考 pad=8000）**——边界约定误差首次来自独立参考（绝对差+相对差并报，相对尺度=长参考窗内最大幅度）：

| τ | BC ampRatio/posBias（p 路） | BC \|Δp\|abs / rel | BC \|Δp′\|rel | BC \|Δb\|rel |
|---|---|---|---|---|
| 400 | 1.00000000 / +0.0000 | 1.018e-09 / 2.08e-09 | 6.18e-10 | 4.15e-11 |
| 600 | 1.00000000 / +0.0000 | （JSON）/ 8.08e-14 | 1.46e-14 | 1.43e-14 |
| 800 | 1.00000000 / −0.0000 | （JSON）/ 7.7e-14 | 1.15e-14 | 1.1e-14 |
| 1200 | 1.00000000 / +0.0000 | （JSON）/ 2.21e-19 | 1.39e-18 | 1.66e-18 |
| 2700 | 1.00000000 / +0.0000 | 7.0e-14 / 1.43e-13 | 1.85e-14 | 1.9e-14 |

   τ=400 的 |Δp|abs=1.018e-09 与"触发前无激励模型下回波包络在 τ=0 的 ~1e-9 裙边"一致——稳态起点线的边界约定误差真实存在、量级 ~1e-9 以下，但**不再以精确 0 表述**；INT 列（burstEcho vs echoOnly，同窗对比）与 9ace9e1 数值一致（该对比本非自比）：τ=400 p 路 4355.96、τ=2700 仍 9.25。
6. **滤波组合 × 反演覆盖（B6.combos，echoOnly τ=800 整线）**：none 行 p 路逐位一致（|Δ|=0）、p′/b 内部区逐位一致（首/末样本存在单侧 vs 中心差分的**真实端点约定差异**，与 exp3 T7 线端探针同源，JSON combosNote 注明）；HP 7.7e-14、LP 7.0e-38、HP+LP 7.7e-14（p 路相对差；滤波组合的 ~1e-13 为长数组双精度级联舍入累积 ~√N·eps·|信号| 量级）。反演单独开启 = none 行 b 路（无滤波反演，|Δ|=0）。
7. **非默认 sysDelay（B6.wl2，D=371）**：|Δp|rel=1.08e-17、|Δp′|rel=2.04e-17、|Δb|rel=2.55e-17。
8. **起端/终端敏感性（B6.sensitivity，测试 S5）**——有限窗边界效应**被真实测出**（非平凡输入）：
   - 起端（burstOnly，burst 起点即裁剪起点，窗 [0,1500]）：|Δp|abs=532.8（burst 尺度 1712）、rel=**0.311**——短窗奇延拓镜像上升沿 vs 长参考真实触发前静默，差异显著；
   - 终端（echoOnly τ=3600，距裁剪线末端 43 样本）：ampRatio=**1.246209631**、posBias=**+0.6675 样本**、|Δp|rel=**0.633**、|Δb|rel=0.136——短窗尾部奇延拓镜像 ~0.8 幅度的回波拖尾 vs 长参考真实延续，近尾目标被高估 ~25%。
   - 结论含义（待审）：无干扰稳态起点线的边界约定误差 ≤1e-9 量级；**burst 污染起点与近尾目标是边界约定误差的真实来源**（数据相关），这取代了旧"边界约定误差精确 0"的表述——"不采纳 848–3639 全局丢弃"的结论维持，但依据从自比零误差改为上述独立参考量化。

**结论变化**：旧 B6 全部 BC 数值（bcRatio≡1、bcBias≡0）作为自比伪影**撤回**；新 BC 数值来自收敛独立参考（结构断言+负例+收敛+敏感性四重支撑）。旧 `res.policy.headPolicy` 的"measured ~0"依据同步撤回并改写（exp5 JSON policy 字段）。"不机械丢弃 848–3639"与"dbrEnd<sysDelay 拒绝"两项策略结论维持、证据更换。

---

## B2（P1）时间轴规格遗漏 sysDelay−1 —— 已统一收口（文档/回执/JSON/测试）

**缺陷确认**：`algorithm-stage-A.md:94–95`（§2.2 表）与 `review-remediation.md:153–157` 将未裁剪查询位置写为 `τ₀ = d·fs/c`（物理走时样本数 s），同时声称与裁剪线取到同一 raw 样本（"原始连续坐标 τ₀+1 = τ+sysDelay"）——该等式要求 τ₀ = s+sysDelay−1，表格遗漏 `sysDelay−1`。实际 `exp3_time_axis.m:195`（`tau0Q = tauQ + sD - 1`）实现正确。这是规格错误，不是生产代码 bug；按任务要求修正文档去匹配正确脚本，未改 exp3 的正确 offset。

**统一记号（全篇采用，algorithm-stage-A.md §2.2）**：

```
D = 每通道/波长 systemDelay（raw 一基声学零点）
T = 物理传播时间 [s]（含既有直线分区双声速计算）
s = fs·T（物理走时样本数，连续）
m = raw 一基连续样本坐标；q = 当前存储数组的零基连续查询坐标

raw 声学时间：t(m) = (m − D)/fs
裁剪线（delayCut=1）：q_cut = s，取值原始坐标 m = q_cut + D = s + D
未裁剪线校准查询（delayCut=0）：q_raw = s + D − 1，m = q_raw + 1 = s + D
反演乘子时间：t = T = s/fs = (q_raw + 1 − D)/fs
旧 DAS 未裁剪查询 q_legacy = s（无补偿）= 历史兼容行为，单列 §2.3，不混入拟议反演表格
```

**修复位置**：

- `algorithm-stage-A.md` §2.2 表格重写（两态校准查询/声学时间/取值位置三列，q_cut=s 与 q_raw=s+D−1 并列）；§2.3 改写（旧行为差距量化保留 + 阶段 B 集成建议确定化，见下）；新增 §2.5 三个功能组合的查询兼容性表；§0.5 摘要、§7 集成要点同步。
- `review-remediation.md` "两态查询公式（冻结约定，R3）"块修正（原矛盾文本以勘误注记标明，原文见 git 历史 9ace9e1）。
- `exp3_time_axis.m`：头部索引约定注释改用统一记号；"生产语义核对"改为二轮收口建议。
- `exp5_order_endpoints.m`：res.policy 新增 `twoStateQuery` 字段（同一记号）。

**新增测试证据（exp3，`evidence/exp3_time_axis.json`）**：

- **T5 独立 p′ 两态差**（此前仅由 b 间接覆盖）：max|Δp′| = 5.83e-06（相对 |p′|max 5.264e+05 → **1.11e-11**）。残差来源：q_raw = s+D−1 在双精度下的查询坐标舍入（分数部分 ~1e-13 样本 × p′ 局部斜率），与 p 两态差（9.77e-15）同源；断言容差 1e-10 相对（JSON `statePPNote` 注明）。T6 分层走时独立 p′ 两态差 = 0。
- **T7 坐标恒等式断言【新】**：q_cut/q_raw/m/t 链（m_raw = m_cut、t(m)=(m−D)/fs = T = (q_raw+1−D)/fs）对 D∈{358,371,359} × s=[700, 703.64, 1677.85, 1090.60, 2400]（整数+亚样本）全部成立，max 误差 **4.24e-22** 样本（浮点容差 1e-9）。
- **T7 线端导数约定探针【新】**（burstOnly，信号在 τ=0 非零）：s=0 → |Δp′|=1.253e+10、s=0.5 → 6.264e+09（裁剪线单侧差分 vs 全行中心差分的**真实端点约定差异**，记录不判 0、非 bug；b 在线端继承）；s=1.5 → **0**（两态共享同一 raw 邻域，逐位一致）。
- 既有 T1–T6 结论不变（两态 p/b 差 ≤9.8e-15/3.1e-11、跨通道 0、不补偿偏差 0.985、±1 可分辨）。

**确定的集成建议（取代"阶段 B 任选其一"，待规划审查）**：保留两态反演，未裁剪线使用补偿查询 **q_raw = s+D−1（等价 tf_eff = tf + sysDelay − 1，覆盖分层修正后统一加）**；生产 delayCut=0 旧行为（q_legacy=s，不补偿）与反演 b 的时间乘子不相容（exp3 T5：max|Δp|=0.985 ≈ 错位 (D−1)·c/fs ≈ 2.13mm），不得静默沿用。若规划审查改选"反演仅允许 delayCut=1"，必须**显式报错**而非自动切换，未裁剪两态测试仅作数学坐标参考。三个功能组合的兼容性（§2.5）：

| 组合 | delayCut=1 | delayCut=0 | 兼容性 |
|---|---|---|---|
| 全部关闭（现生产） | q_cut=s（现状） | q_legacy=s（旧行为，样本 1=声学零点的自洽约定） | 完全保持，不变 |
| 仅 HP/LP | q_cut=s（现状） | q_legacy=s（维持旧查询几何——滤波只改信号内容不改索引，旧约定整体自洽） | 不因仅开滤波改变旧 DAS 查询（如未来需改变，须显式返回兼容差异） |
| 反演开启（±HP/LP） | q_cut=s（现状） | **必须补偿 q_raw=s+D−1**（或反演强制 delayCut=1 并显式报错） | 不补偿的旧行为与 b 不相容，不得静默沿用 |

> **三轮 C3 注记（20260920）**：上表第三行 delayCut=0 列的"或反演强制 delayCut=1 并显式报错"替代选项与下段"滤波+delayCut=0+DBR 视为暂不支持，留待阶段 B 显式决策"已由三轮收口（任务 `环形成像阶段A收敛判据与配置规则最终收口_20260920-101451.md`）**取代**：唯一查询规则 = 只有反演开启才使用校准未裁剪查询 q_raw=s+D−1（无替代选项）；滤波+delayCut=0+实际置零 E>0 **明确报错拒绝**（不再是待决项）。现行结论以 algorithm-stage-A.md §2.5/§3.3（C3/C2 版）与 review-final-closure.md 为准。

**dbrEnd<sysDelay 拒绝策略的适用边界（任务 §7.2 问项，algorithm-stage-A.md §3.3 同步；本段"留待阶段 B 显式决策"已被三轮 C2 冻结规则取代，见上方注记）**：DBR 关闭 → dbrEnd 解释为 0（无置零段、无阶跃，检查平凡满足；证据：zeroRows=313 vs 0 裁剪线逐位一致，G3）；仅反演（无 HP/LP）→ 无零相位非因果回卷，不要求该拒绝，有效查询规则 m ≥ dbrEnd+1 仍排除置零前缀；delayCut=0 且 DBR 开启 → 置零前缀**构造上就在滤波输入内**（无裁剪），"阶跃在裁剪区外"的依据**不适用**于未裁剪线，滤波+delayCut=0+DBR【三轮 C2：明确报错，不再留待阶段 B】；短窗（线长≤3n）报错（A5）、越界查询 maskOob 置零、端点导数单侧差分（T7 探针量化）均维持。

---

## B3（P2）exp2 JSON modes 混入图像矩阵 —— 已修复 + 结构断言 + 数值不变核对

**缺陷确认**：`exp2_ring_ubp_2d.m:229` `struct('modes', {modes(1,:)}, ...)` 取 5×2 cell 的**第一行**（'das' + 图像 cell），JSON `centerPosErrMmByMode.modes` 混入 4 个 181×181 图像矩阵（这也是旧 JSON 达 4.7MB 的原因）。

**修复位置**：`exp2_ring_ubp_2d.m`——改为名称列 `modes(:,1)`；新增 `gate.modeNameList` 结构断言（iscell + 全部 ischar + 数量=5）并纳入 gateOk；头注注明仅影响 JSON 打包。

**验证**：重跑 exp2（退出码 0，76.1s）；与 9ace9e1 版 JSON 对比（jsondecode 后 `isequaln`）：**除 modes 字段外全部字段逐位一致（=1）**，`posErrMm` 一致（=1）——**数值不变**，仅序列化结构修正；新 `modes = ['das','legacyW','ubpD','ubpP','saP']`；JSON 体积 4.71MB → 40KB。已审过的重建计算零改动。

---

## 证据重生成清单与未跑项

| 文件 | 状态 |
|---|---|
| evidence/exp5_order_endpoints.json/.mat | **重生成**（B6 重写；B1/B2/B4/B5/B7/A5 数值与 9ace9e1 一致——B2 表逐值核对：置零段输出 396/562/296、阶跃差 2074/2032、τ=2737/2734 不变） |
| evidence/exp3_time_axis.json | **重生成**（T5 p′ 差、T6 p′ 差、T7 新增；T1–T4 原值不变） |
| evidence/exp2_ring_ubp_2d.json、exp2_images.mat | **重生成**（B3 打包修正，数值逐位不变；exp2_images.mat 仍为 6.3MB 瘦身版） |
| evidence/test_boundary_reference.json | **新增** |
| evidence/test_time_derivative.json、test_fair_comparison.json | 重跑（R1/R2 未改，通过） |
| evidence/exp1_ubp_sphere.json、exp4_dual_layer.json、exp6_filter_reference.json、filter_reference_vectors.mat | **代码未改**（exp1/exp4/exp6 本轮零改动），经统一入口重跑复核断言全通过（球内 6.39e-08、分层 3.171e-17 m、exp6 交叉验证与 F5 记忆长度表与 9ace9e1 一致） |

## 复跑命令回执

```
命令：matlab -batch "run_all_remediation"（工作目录 = CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/matlab）
MATLAB R2023a 9.14.0.2206163，Windows；本轮退出码 0，汇总 0 项失败，总耗时 ~91 s

[RUN_ALL] test_time_derivative.m    (R1 导数自动测试)                    OK  0.5s
[RUN_ALL] test_fair_comparison.m    (R2 公平对照自动测试)                OK  0.4s
[RUN_ALL] test_boundary_reference.m (二轮B1 边界参考结构/收敛/负例测试)   OK  0.4s
[RUN_ALL] exp1_ubp_sphere.m         (exp1 UBP 常数（未改码，重跑复核）)   OK  0.2s
[RUN_ALL] exp2_ring_ubp_2d.m        (exp2 修复后完整重跑)                OK 76.1s
[RUN_ALL] exp3_time_axis.m          (exp3 时间轴 + 两态反演（重跑）)      OK  0.1s
[RUN_ALL] exp4_dual_layer.m         (exp4 分层走时（未改码，重跑复核）)   OK  1.1s
[RUN_ALL] exp5_order_endpoints.m    (exp5 顺序/边界策略（重跑）)          OK  0.3s
[RUN_ALL] exp6_filter_reference.m   (exp6 滤波参考（未改码，重跑复核）)   OK  0.3s
```

单项命令（均退出码 0）：`matlab -batch "run('test_boundary_reference.m')"`、`run('exp3_time_axis.m')`、`run('exp5_order_endpoints.m')`、`run('exp2_ring_ubp_2d.m')` 等。

## 被本轮回执取代/撤回的先前结论

1. **撤回**（review-remediation.md R4 节 / algorithm-stage-A.md §0.6/§3.3）："B6 BC：τ=400/600/800/1200/2700 全部 ampRatio=1.000000、posBias=0——边界约定误差精确 0"。该"参考"在滤波前退化为与被测相同的输入（B6_filter_input_identical=1, maxdiff=0）。取代者：B6 收敛独立参考量化（稳态起点线 ≤1e-9 量级、burst 起点与近尾目标存在 0.1–0.6 量级有限窗效应）。
2. **撤回**（review-remediation.md"两态查询公式（冻结约定，R3）"块）：delayCut=0 查询 `τ₀ = d·fs/c` 与"两态对应同一原始取样位置"并述——遗漏 sysDelay−1。取代者：统一记号 q_raw = s+D−1（algorithm-stage-A.md §2.2；exp3 T7 恒等式断言）。
3. **取代**（review-remediation.md 未解决决策点 #2 / algorithm-stage-A.md §2.3/§7）："delayCut=0 适配方式（补偿查询还是强制 delayCut=1）由阶段 B 决定并测"。取代者：**确定建议 = 补偿查询 q_raw=s+D−1**；替代方案（仅允许 delayCut=1）如被选必须显式报错，不再"任选其一"。
4. **重定义**（exp5 gate G3）：原"默认配置（313）与自身参考逐位一致"为恒等自比断言；重定义为"DBR 置零段完全在裁剪起点之前时 DBR 开/关裁剪线逐位一致（zeroRows=313 vs 0）"——同时作为"DBR 关闭时 dbrEnd 解释为 0"的证据。
5. **不变**：R1/R2 修复与公平对照结果、R3 两态等价与不补偿差距量化、R4 顺序/实现归因、dbrEnd<sysDelay 拒绝策略、"不采纳 848–3639 全局丢弃"、权重/归一化待审决策点——均保留（证据字段名变化处已在 §字段映射 更新）。

## 主要结论 → 脚本/断言/数据字段映射（本轮更新）

| 结论 | 脚本 | 断言/字段 |
|---|---|---|
| 长参考结构（更长/包含逐位/同原点/额外段滤波时存在） | test_boundary_reference S1；exp5 G1 | ranges.*、b6ref.longContainsShortBitwise |
| 旧自比构造被识别 | test_boundary_reference S2；exp5 G1 | oldConstruction.pad*_maxDiff == 0 |
| pad 收敛（容差记录） | test_boundary_reference S3；exp5 G1b | convergence.(scn).convergedPad / convTolRel |
| 物理时间对齐（无平移/导数原点正确）+ 错位/重归零负例可发现 | test_boundary_reference S4/S2b/S2c | farFieldAlignment.*、misalign.*、derivOrigin.* |
| 起端/终端有限窗效应可测 | test_boundary_reference S5；exp5 B6.sensitivity | sensitivity.startBurstRelP/endEchoRelP |
| 边界约定误差量化（echoOnly 近边界） | exp5 B6.rows | bcAbsMax*/bcRelMax*、bcRatio*/bcBias* |
| 组合覆盖（none 逐位、HP/LP/HP+LP、反演单独=none b 路） | exp5 B6.combos | combos.absMaxP/absMaxPPInterior/absMaxBInterior |
| 两态坐标恒等式（q/m/t 链） | exp3 T7 | maxCoordIdentityErr、pass.T7_coordIdentity |
| 独立 p′ 两态差 | exp3 T5/T6 | maxStateDiffPP、stateDiffPPRel、statePPNote |
| 线端导数约定差异（记录不判 0） | exp3 T7 | endpointProbe、endpointNote |
| JSON 模式名结构 | exp2 | gate.modeNameList、centerPosErrMmByMode.modes |

## 未验证项 / 限制

- 长参考的"触发前无激励"（τ<0 恒 0）是**合成模型约定**；实机是否能提供触发前样本、真实 pre-trigger 历史形态 UNVERIFIED。长参考仅用于隔离边界效应的合成证据，不以"理想"自称（收敛性由 pad 序列实证）。
- 近尾目标（B6.sensitivity.endEcho）与 burst 起点的边界效应数值是**合成参数下**的边界；实机适用范围 UNVERIFIED。
- 生产 delayCut=0 的补偿查询为**建议**（待规划审查），本阶段未修改生产代码，端到端行为未实现。
- b 解析误差仍仅报告不判定（σ=2 应力脉冲的亚样本导数插值误差，两态共享；维持 R3 限制表述）。
- 本轮无生产代码改动，不声称生产算法回归；B/C 未开始；权重/归一化选择仍是独立审查项。

## 阶段 B/C

**仍未开始。** 本次仅触及 `CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/` 内参考/测试/证据/文档；是否 APPROVE 由规划主代理独立复审决定。
