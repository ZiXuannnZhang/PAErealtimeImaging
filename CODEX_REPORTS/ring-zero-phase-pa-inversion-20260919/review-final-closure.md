# 阶段 A 三轮收口回执：收敛判据与配置规则（C1/C2/C3）

任务：`codex/task-docs:TASKS/环形成像阶段A收敛判据与配置规则最终收口_20260920-101451.md`（REQUEST_CHANGES，第三次审查整改追加；任务发布 SHA `f609448382029295b6dd8f35451221b366dd58df`）
分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
starting SHA：`63e088a25a67e941d710be79ca501b3904801bc7`（= 任务指定起点，fetch 后已核对 `git rev-parse HEAD`）
final SHA：见本提交；推送后 local HEAD == remote HEAD == ls-remote 三处核对结果附于执行报告。
环境：MATLAB R2023a（9.14.0.2206163）+ Signal Processing Toolbox，Windows。
提交方式：新增提交（未 amend/rebase/squash/force push），未合并 main，未开始 B/C，未修改生产代码（全部改动限于 `CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/`）。
上位任务与既往回执：`环形成像零相位滤波与光声反演滤波_20260919-024359.md`、`环形成像阶段A审查整改追加_20260919-201935.md`（R1–R5）、`环形成像阶段A边界参考与时间轴规格收口_20260919-235753.md`（B1–B3）、[review-remediation.md](review-remediation.md)、[review-spec-closure.md](review-spec-closure.md)。

统一复跑入口：`matlab -batch "run_all_remediation"`（工作目录 = `matlab/`），本轮最终运行 **11/11 项 OK、汇总 0 项失败、进程退出码 0**（复跑记录见文末）。

---

## C1（P1）收敛判据允许参考自比导致必然通过 —— 已重写为统一判据 + 正负例

**缺陷确认**：63e088a `matlab/test_boundary_reference.m:118–129` 与 `matlab/exp5_order_endpoints.m:316–319`（行号对应任务 §4 引用 525–527 附近的收敛块）把 padRef=8000 放入 padList 候选并与自身比较（relP/relPP/relB 的 padRef 行恒为 0），门条件仅 `convergedPad <= padRef`——即使其余 pad 全部超差（误差表只有 padRef 行为 0）也会通过。判据对"其他 pad 是否收敛"无辨别能力。实际 pad 曲线本身呈趋近参考的趋势（二轮已实证），本项不否定真实长参考构造；修复的是自动判据的辨别能力。

**修复位置**：

- **`matlab/padConvergenceCheck.m`【新】**——pad 收敛判据唯一入口（test_boundary_reference S3 与 exp5 B6 共用，避免两份标准漂移）。通过标准全部满足才 PASS：
  1. padRef 只作参考，pad==padRef 项禁止作为通过候选（自比恒零不计入）；
  2. 至少两个不同的非参考 pad 与参考的 p/p′/b 误差均在容差内（容差维持记录值 1e-9 相对窗内幅度，未放宽容差）；
  3. 通过的 pad 两两之间稳定（真实数据路径用**实际两两差**——同一参考尺度下不同 pad 结果之间的最大相对差；人工误差表路径可用三角不等式保守上界 rel_i+rel_j）且从首个通过 pad 到最大非参考 pad 不出现再次超差（稳定尾段，relapse 检查）；
  4. 候选不足（<2）、误差非有限（NaN/Inf）、长度/形状不合法、全部候选超差一律 FAIL——不允许空数组 all() 真值通过；
  5. 最大参考是否足够仍是数值近似结论（由多个非参考窗口的稳定尾段支持），不宣称严格数学证明——输出 detail 结构含 firstOkPad/nOk/maxPairRel/adjPair*/relapse 全量审计字段。
- **`matlab/test_boundary_reference.m` S3 重写**——收敛块改调 padConvergenceCheck（真实两两差矩阵 pairRel 路径）；新增 **S3n 判据正负例**（人工误差表直接喂判据，不重跑完整前向，每例 ~µs 级）。
- **`matlab/exp5_order_endpoints.m` B6 收敛块重写**——同一判据入口；门 `G1b_refConverged` 改为三场景 verdict 全 PASS（旧门 `all(convPads<=padRef)` 撤回）；`convergedPad` 字段语义改为 firstOkPad（JSON 兼容保留字段名，值为新判据首个通过 pad）。
- 旧自比样本（padRef 行误差恒 0）在输出中保留作为诊断曲线，明确不计入验收（detail.candIdx/okIdx 可审计参考行被排除）。

**判据正负例证据**（`evidence/test_boundary_reference.json` `criterionNegatives`，全部断言 PASS）：

| 用例 | 构造（人工误差表） | 预期 | 实际 |
|---|---|---|---|
| N1 仅参考自比通过、其余全超差 | rel=[1e-3,1e-2,1e-1,1,10,**0**]（padRef 行=0） | **FAIL** | FAIL（"全部非参考候选超差"）——旧判据在此会通过，新判据拒绝 |
| N2 仅一个非参考 pad 通过 | rel=[1e-12,1e-3,1e-2,1e-1,1,0] | **FAIL** | FAIL（"仅 1 个非参考 pad 通过（需 ≥2）"） |
| N3 两个较小 pad 通过但更大非参考 pad 再次超差 | rel=[1e-12,1e-12,1e-12,5e-2,1e-1,0] | **FAIL** | FAIL（"尾段再次超差（pad=2000 后出现超差候选）"） |
| N4 NaN/Inf | rel=[1e-12,NaN,1e-12,1e-12,Inf,0] | **FAIL** | FAIL（"relP 含 NaN/Inf"） |
| N5 没有候选 | padList 仅含参考单个值 | **FAIL** | FAIL（"无非参考候选"） |
| N5b 形状不合法 | 误差表长度与 padList 不一致 | **FAIL** | FAIL（"relP 与 padList 长度不一致"） |
| P1 ≥2 非参考 pad 稳定尾段 | rel=[1e-12,1e-12,1e-13,1e-13,1e-14,0] | PASS | PASS（firstOk=250 nOk=5） |
| P2 参考行不参与判定 | 同 P1 但 padRef 行人为填超差值 5e-1 | PASS | PASS（锁定"padRef 行被排除"语义） |
| P3 最小合法用例（双候选） | pad=[2000,4000,8000], rel=[1e-12,1e-13,0] | PASS | PASS（firstOk=2000 nOk=2） |

预期失败用例（N1–N5b）的"FAIL"是被测判据的正常拒绝结果；套件断言 = 判据返回值与预期一致，全部 9 例通过（`pass.S3n_*` 共 9 项全 true）。套件整体成功不掩盖任何用例——若有任一判据返回与预期不符，`assert` 非零退出。

**真实 pad 曲线按新判据重算**（`evidence/test_boundary_reference.json` `convergence` + `evidence/exp5_order_endpoints.json` `B6.reference.convergence`；各 pad 相对误差数值与 63e088a 一致——同一前向数据，仅判据更换）：

| 场景 | verdict | firstOkPad | nOk | maxPairRel（实际两两差） | relapse |
|---|---|---|---|---|---|
| echoOnly | PASS | 250 | 5 | 6.6e-17 | 无 |
| burstOnly | PASS | 500 | 4 | 7.2e-10 | 无 |
| burstEcho | PASS | 500 | 4 | 7.2e-10 | 无 |

误差明细（test_boundary_reference，目标 τ=800 窗 [300,1300]，相对 pad=8000 参考窗内幅度）：echoOnly relP=[1.4e-17,1.4e-17,8.9e-18,1.4e-17,8.9e-18,(padRef 行)]；burstEcho relP=[2.3e-9,7.2e-10,2.3e-11,1.1e-13,2.8e-14,(padRef 行)]——候选 5 个非参考 pad 中 echoOnly 全过、burstEcho 4 过（250 超差），首个通过起无回落。相邻通过 pad 两两差 JSON 字段 `detail.adjPairP/adjPairPP/adjPairB`。

**起端/尾端窗口稳定性核对**【四轮注记（任务 `环形成像阶段A最小收尾与方向校准_20260920-122834.md`）：下述 77b1d37 版段落把中心区间收敛与人工正例当作端点证据，且把参考 pad 8000 写成非参考 pad，**已撤回**；原文见 git 历史 77b1d37。取代者：`test_endpoint_stability.m`（见本文件末"四轮收尾补测记录"）——起端 burstOnly 实际报告区间（MATLAB 索引 1:1500 = 0 基声学坐标 τ=0..1499）与近尾 echoOnly τ=3600 实际计算区间（MATLAB 3100:3643 = 0 基 τ=3099..3642，上限被短窗线末截断，不是未截断的 [3100,4100]）各自以非参考 pad {2000,4000} vs 参考 pad 8000 实测：两场景 padConvergenceCheck 判据均 PASS（firstOkPad=2000、nOk=2、maxPairRel=1.25e-13 / 9.38e-14 < 1e-9，无尾段回落），各 pad 与参考的 p/p′/b 相对差及两候选窗差均在 1e-13 量级；原敏感性数值在稳定参考下逐位不变（短窗 vs 8000 行 |Δp|rel=0.31117044891792733 / 0.63275363030561171、|Δb|rel=0.13554445421030059 与 77b1d37 已发布值相对差恰为 0）。8000 是参考 pad。维持二轮起端 0.311/近尾 0.633 有限窗边界效应量化表述与实机 UNVERIFIED——补测检验的是长参考稳定性，不是要求短窗与长窗误差小或为零。证据：`evidence/test_endpoint_stability.json`（MATLAB R2023a 9.14.0.2206163，退出码 0）】。

~~（任务 §7.C1 第 5 条——对已有起端/尾端敏感性结果在其报告区间用至少两个不同长窗口核对）：起端 burst 场景窗 [0,1500] 与近尾 τ=3600 窗 [3100,4100] 的短窗 vs 长参考差异本身即由 padRef=8000 长窗计算（S5/b6sens），另经 S3n P1/P2 与真实曲线 maxPairRel（两个最大非参考 pad 4000 vs 8000 的实际差 7.2e-10 < 容差）确认该结论在 ≥2 个不同长窗口下稳定——不宣称"所有边界结果已收敛"，维持二轮起端 0.311/近尾 0.633 的量化表述与实机 UNVERIFIED。~~【撤回原因：其引用的收敛证据（S3，中心窗 [300,1300]）与正例（S3n 人工误差表，检验判据本身）均不覆盖起端/尾端窗口；近尾窗写成未截断的 [3100,4100] 与实际计算区间（MATLAB 3100:3643）不符；"两个最大非参考 pad 4000 vs 8000"中的 8000 是参考 pad 而非非参考候选。本补测不改变 C1 判据本身（S3/S3n 证据与结论维持），只更换端点稳定性证据的归属与数值来源。】

**结论变化**：63e088a 的"convergedPad=250/500 ≤ padRef=8000"通过记录**不是**新判据证据，已由上述重算取代；自比候选参与判定的旧逻辑撤回。保留独立长参考构造、物理时间对齐与全部已通过 R1/R2/B1 修复（未重做）。

---

## C2（P1）不支持配置组合留给阶段 B 且缺拒绝测试 —— 已冻结规则 + 矩阵测试

**缺陷确认**：`algorithm-stage-A.md`（63e088a §3.3"DBR/两态适用边界"行与 §7 第三条）、exp5 `res.policy.validQueryRegion`、review-spec-closure.md B2 节均保留"滤波+delayCut=0+DBR 暂不支持，留待阶段 B 显式决策"，且 E 语义（开关 vs 实际置零长度）未定义、无逐通道规则、无任何可执行测试。

**修复位置**：

- **`matlab/filterDbrConfigCheck.m`【新】**——配置前置校验参考实现（仅本任务新增处理定义；当前只做参考校验与测试，**不接入生产 UI**）。冻结规则（逐启用通道/波长检查，一条不满足即拒绝本组配置并报告具体通道名与原因，不只检查全局默认 D）：
  - 定义：filterEnabled = highpassEnabled‖lowpassEnabled；inversionEnabled = 反演开关；D = 该通道/波长 systemDelay；**E = 实际 DBR 置零样本数**（真实预处理语义：DBR 关闭→E=0；DBR 开启但有效置零长度为 0→E=0，开关开启不等于存在阶跃；E = min(maskLength+dbrmaskExtra, Nt)，计入该波长 dbrmaskExtra 与实际样本数边界，不混用配置值与实际长度）。
  - 规则表：filterEnabled=false → 不触发零相位滤波专属拒绝（仅反演保持既定有效查询/导数端点规则）；filterEnabled=true 且 E=0 → DBR 相关规则允许（采样率/截止/阶数/长度校验仍由 refZeroPhase 既有报错承担）；filterEnabled=true 且 E>0 且 delayCut=true → 仅当 E<D 允许，E≥D 明确报错；filterEnabled=true 且 E>0 且 delayCut=false → **明确报错，本版不支持该组合，不再留给阶段 B 任选**。
  - 错误语义（参考文案）："零相位滤波与未裁剪的 DBR 置零前缀不兼容，请启用延时裁剪或取消 DBR 置零。"（FILTER_UNCUT_DBR_UNSUPPORTED）/"DBR 置零末端必须早于该通道/波长的延时裁剪起点。"（DBR_END_NOT_BEFORE_DELAYCUT_START）。
  - 无效参数（maskLength<0、D≤0、Nt 非正整数等）沿用既定校验语义直接报错，不截断静默修复；全部新功能关闭时不引入新拒绝（旧行为保持）。
- **`matlab/test_filter_dbr_config.m`【新】**——矩阵测试，纳入 `run_all_remediation.m`。
- 文档同步：algorithm-stage-A.md §3.3（DBR/两态适用边界行整行重写为 C2 冻结规则）、§0.6、§7、§8.6；exp5 `res.policy.validQueryRegion`（FROZEN C2 rule table 全文）；review-spec-closure.md B2 节"留待阶段 B"以三轮注记标撤回。

**测试证据**（`evidence/test_filter_dbr_config.json`，全部断言 PASS）：

- **配置矩阵 80 用例**（M2，全部 expected==actual）：HP/LP/HP+LP/none（filterEnabled 构成）× 反演开/关 × delayCut true/false × E 构造 {DBR 关→E=0；L=0→E=0；E=D−1=357；E=D=358；E>D=359}，D=358（wl1）。结果：50 接受 / 30 拒绝；拒绝原因仅上述两种且逐用例记录（`rows[].reason`）。
- **M1 反演无关性**：同 (滤波, delayCut, E) 下 inv 开/关的判定一致（断言 agreeInv）——反演单独开启不因本表触发 HP/LP 限制。
- **M3b 边界值**（D=358 与 wl2 D=371）：E=D−1 接受 / E=D 拒绝 / E>D 拒绝（delayCut=true）；wl2 专属对（E=370 接受/E=371 拒绝）通过——不只检查全局默认 D。
- **M4 逐通道**：wl1 合法 + wl2 非法（E=400>371）→ **整组拒绝**且仅报告 wl2-bad（含通道名与原因）；全好组接受。
- **M5 E 真实语义**：DBR 开但 maskLength=dbrmaskExtra=0 → E=0 → delayCut=false 也接受（开关开启 ≠ 存在阶跃）；L=5000>Nt=4000 → E 截为 Nt=4000 ≥ D → delayCut=true 拒绝（计入实际样本数边界）。
- **M6 旧行为保持**：全部新功能关闭时任意 DBR/delayCut 组合不因本表拒绝（8 组合全接受）；仅反演开启（无滤波）接受。
- **M7 非法参数**：maskLength=-1 / sysDelay=0 / Nt 非整数 → filterDbrConfigCheck 直接 error（沿用既定校验，3 例全捕获）。

**边界说明（不扩大验收）**：通过本配置校验**不构成**无边界误差或目标无损保证——已量化的起端/尾端有限窗误差（B6.sensitivity 0.311/0.633）与实机 UNVERIFIED 限制原文保留；C2 仅是"组合是否允许"的前置规则。

---

## C3（P1）"仅高低通"是否改变查询坐标表述矛盾 —— 唯一查询规则收口 + 策略测试

**缺陷确认**：`algorithm-stage-A.md:292`（63e088a §7 第二条）前半句"反演/滤波启用时，未裁剪线查询采用补偿 q_raw = s + D − 1"与后半句"三组合兼容性按 §2.5：全部关闭与仅 HP/LP 均维持旧查询几何不变，仅反演开启触发适配"矛盾——"仅 HP/LP"是否触发补偿存在两种执行解释。§2.5 表第三行还留有"或反演强制 delayCut=1 并显式报错"替代选项；§2.3 有同源表述；review-spec-closure.md B2 节同；exp3/exp5 头注与 policy 同。

**修复位置**：

- **`matlab/test_query_policy.m`【新】**——三组合查询规则参考策略测试，纳入 `run_all_remediation.m`。
- **规则统一**（algorithm-stage-A.md §2.5 重写 + §0.5/§2.3/§7/§8 同步；exp3 头注"生产语义核对"与 `params.productionDelayCut0`；exp5 `res.policy.twoStateQuery`）：

| 功能组合 | delayCut=true | delayCut=false |
|---|---|---|
| 全部关闭 | q=s，保持旧行为 | q_legacy=s，保持旧行为 |
| 仅 HP/LP（反演关） | q=s，保持旧行为 | q_legacy=s，保持旧行为；先通过 C2 组合校验 |
| 反演开启（±HP/LP） | q_cut=s，m=s+D | q_raw=s+D−1，m=q_raw+1=s+D；先通过 C2 组合校验 |

- **删除全部"反演/滤波启用时都补偿"歧义**：改为"只有反演开启才使用校准的未裁剪查询"。反演乘子恒为 t=T=s/fs（**不是**把 q_raw 直接除 fs）。本表为本轮确定建议，**不再提出"强制 delayCut=1"替代选项**；未来范围变更需单独审查。
- 分层走时规则：先算完 s（含双声速分区修正）再加索引偏移 D−1（分层修正只进入 s，不改变偏移项）。
- 历史回执处理：review-spec-closure.md B2 表第三行与适用边界段、review-remediation.md"两态查询公式"块的替代选项表述以三轮注记就地标撤回（原文见 git 历史 63e088a）；正文引用旧结论处均标为已撤回/被取代。

**测试证据**（`evidence/test_query_policy.json`，Q1–Q6 全 PASS；复用 exp3 既有数值锚点 D∈{358,371,359}、s=[700,703.64,1677.85,1090.60,2400]，不重复实现重建核）：

| 项 | 断言 | 结果 |
|---|---|---|
| Q1 仅 HP/LP 不改变 q | 滤波开关四态（none/HP/LP/HP+LP）在两种 delayCut 下查询坐标全部相同（q=s / q_legacy=s）——查询几何与滤波开关完全解耦 | PASS |
| Q2 反演未裁剪偏移恰为 D−1 | q_raw−s = D−1 对 3 通道 × 5 走时（含亚样本）成立，max 偏差 1.14e-13（(s+D−1)−s 双精度求和舍入，同 exp3 T7 恒等式性质；断言容差 1e-9）；裁剪线 q_cut=s 零偏移 | PASS |
| Q3 分层算完再加偏移 | 分层 s_L（L1=4.0000mm，c1/c2=1490/1540）与未裁剪 q_raw=s_L+D−1 满足 m=s_L+D；偏移项仍恰 D−1；同速退化一致 | PASS |
| Q4 乘子恒等 | t=T=s/fs=(q_raw+1−D)/fs=(m−D)/fs 全等（max 4.24e-22）；q_raw/fs 错用与正确乘子之差恰为 (D−1)/fs（实测 1.428e-6 s，断言与解析值 10% 内一致——可分辨非语义等价） | PASS |
| Q5 三组合 × 两态选择唯一 | 32 组合（滤波×反演×delayCut×DBR）查询坐标仅由 (inversion, delayCut) 决定；DBR 参数不改变 q；反演+滤波+delayCut=0+DBR 开 → C2 前置拒绝联动（FILTER_UNCUT_DBR_UNSUPPORTED） | PASS |
| Q6 两态同位取值（数据链） | 同一 raw 中 q_cut 与 q_raw 取到同一 m、同一插值分数（exp3 T7 的独立复算：max pos 差 9.0e-39、frac 差 1.14e-13 ≤ 1e-12） | PASS |

**结论变化**：63e088a §7 "反演/滤波启用时，未裁剪线查询采用补偿"的歧义表述撤回；"或反演强制 delayCut=1"替代选项从所有规范位置移除。已存在的两态导数端点差异仍按 exp3 T7 endpointProbe 既定规则报告（s<1 首样本单侧 vs 中心差分真实约定差异），不承诺所有端点逐位等价——本轮未重跑 exp3 数值链（T1–T7 断言维持 63e088a 证据 + 本次统一入口复跑复核，全部 PASS）。

---

## 证据重生成清单与未跑项

| 文件 | 状态 |
|---|---|
| evidence/test_boundary_reference.json | **重生成**（S3 新判据 + S3n 正负例 9 例；S1/S2/S2b/S2c/S4/S5/S6 数值不变） |
| evidence/test_filter_dbr_config.json | **新增**（C2 矩阵 80 用例） |
| evidence/test_query_policy.json | **新增**（C3 Q1–Q6） |
| evidence/exp5_order_endpoints.json/.mat | **重生成**（B6 收敛块新判据 + G1b 新门 + policy C2/C3 同步；B1/B2/B4/B5/B7/A5/BC/INT/敏感性数值不变） |
| evidence/exp3_time_axis.json | **重生成**（头注与 params.productionDelayCut0 文本 C3 同步；T1–T7 数值断言不变，本次复跑全 PASS） |
| evidence/exp2_ring_ubp_2d.json、exp2_images.mat | 经统一入口重跑重生成（exp2 代码零改动，数值确定性复核；非本轮 C1/C2/C3 依赖项） |
| evidence/exp1/exp4/exp6 JSON、filter_reference_vectors.mat | 代码零改动，经统一入口重跑复核断言全通过（非本轮依赖项） |
| evidence/test_time_derivative.json、test_fair_comparison.json | R1/R2 未改，重跑通过（非本轮依赖项） |

本轮未重跑项：exp1/exp4/exp6 的独立单跑未单独执行（经统一入口整体复跑覆盖，其代码零改动）；exp2 大型图像证据（exp2_images.mat）为统一入口重跑产物，代码零改动、内容约定不变。

## 复跑命令回执

```
命令：matlab -batch "run_all_remediation"（工作目录 = CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/matlab）
MATLAB R2023a 9.14.0.2206163，Windows；本轮退出码 0，汇总 0 项失败，共 11 项

[RUN_ALL] test_time_derivative.m  (R1 导数自动测试)               OK  0.2s
[RUN_ALL] test_fair_comparison.m  (R2 公平对照自动测试)           OK  0.2s
[RUN_ALL] test_boundary_reference.m (二轮B1+C1 边界参考/收敛/负例)  OK  0.2s
[RUN_ALL] test_filter_dbr_config.m (C2 配置矩阵测试)              OK  0.1s
[RUN_ALL] test_query_policy.m      (C3 查询规则策略测试)         OK  0.0s
[RUN_ALL] exp1_ubp_sphere.m         (exp1 UBP 常数（未改码复核）)   OK  0.1s
[RUN_ALL] exp2_ring_ubp_2d.m       (exp2 完整重跑)                OK 50.4s
[RUN_ALL] exp3_time_axis.m          (exp3 时间轴+两态反演)          OK  0.1s
[RUN_ALL] exp4_dual_layer.m         (exp4 分层走时（未改码复核）)   OK  0.6s
[RUN_ALL] exp5_order_endpoints.m    (exp5 顺序/边界策略（C1 判据）) OK  0.2s
[RUN_ALL] exp6_filter_reference.m  (exp6 滤波参考（未改码复核）)   OK  0.2s
[RUN_ALL] 汇总：0 项失败 / [RUN_ALL] 全部通过
```

单项命令（均退出码 0）：`matlab -batch "run('test_boundary_reference.m')"`、`run('test_filter_dbr_config.m')`、`run('test_query_policy.m')`、`run('exp5_order_endpoints.m')`、`run('exp3_time_axis.m')`。

## 被本轮回执取代/撤回的先前结论

1. **撤回**（test_boundary_reference S3 / exp5 B6 收敛块，63e088a）："okPads 取 min 即 convergedPad，门 = convergedPad ≤ padRef"——padRef 自比恒零计入候选，其余 pad 全超差也会通过。取代者：padConvergenceCheck 统一判据（自比排除、≥2 非参考 pad、两两稳定、无再次超差、负例 6 必败）。
2. **撤回**（algorithm-stage-A.md §3.3/§7、exp5 policy、review-spec-closure.md B2）："滤波+delayCut=0+DBR 暂不支持，留待阶段 B 显式决策"。取代者：C2 冻结规则——filterEnabled=true、E>0、delayCut=false **明确报错拒绝**（FILTER_UNCUT_DBR_UNSUPPORTED），不再是待决项。
3. **撤回**（algorithm-stage-A.md §2.5 表/§2.3/§7、review-spec-closure.md B2 表、review-remediation.md 两态查询公式块）："或反演强制 delayCut=1 并显式报错"替代选项与"反演/滤波启用时都补偿"歧义表述。取代者：C3 唯一查询规则——只有反演开启才使用校准未裁剪查询 q_raw=s+D−1；仅 HP/LP 与全部关闭维持旧查询几何；无替代选项。
4. **重定义**（exp5 gate G1b）：旧门"三场景 convergedPad ≤ padRef"改为"三场景经 padConvergenceCheck 判 PASS"。
5. **不变**：R1/R2/B1 全部修复与证据、独立长参考构造（longReference.m）、物理时间对齐、q_raw=s+D−1 校准公式、公平对照与 JSON 名称字段修复、权重/归一化待审决策点、起端 0.311/近尾 0.633 敏感性量化与实机 UNVERIFIED 限制。

## 主要结论 → 脚本/断言/数据字段映射（本轮新增/变更）

| 结论 | 脚本 | 断言/字段 |
|---|---|---|
| 收敛判据排除自比 + ≥2 非参考 pad + 两两稳定 + 无回落 | padConvergenceCheck.m | verdict/detail（firstOkPad/nOk/maxPairRel/adjPair*/relapse） |
| 判据负例全拒绝、正例全通过 | test_boundary_reference S3n | pass.S3n_negative1..5b / S3n_positive1..3、criterionNegatives |
| 真实 pad 曲线新判据重算 | test_boundary_reference S3；exp5 B6 | convergence.(scn).verdict/detail |
| C2 冻结规则表 + 逐通道拒绝 | filterDbrConfigCheck.m + test_filter_dbr_config | accepted/rejects(reason/message)、matrix.rows |
| E 真实语义（开关≠阶跃、Nt 截断） | test_filter_dbr_config M5 | dbrOnZeroLen/eCappedByNt |
| 全功能关闭不引入新拒绝 | test_filter_dbr_config M6 | noneCombo/inversionOnly |
| 三组合唯一查询规则 | test_query_policy Q1/Q5 | Q1 cases、Q5 nCases/unique |
| 未裁剪偏移恰 D−1、分层后偏移、乘子恒等 | test_query_policy Q2/Q3/Q4 | maxOffsetErr/maxErr/maxIdentityErr/wrongUsageGap |
| 两态同位取值独立复算 | test_query_policy Q6 | maxPosErr/maxFracErr |
| 规范/回执/JSON 三处一致 | algorithm-stage-A.md §0.5/§2.3/§2.5/§3.3/§7、exp3/exp5 policy、本回执 | policy.validQueryRegion/twoStateQuery 文本 |

## 未验证项 / 限制

- C2 配置校验为**参考实现与测试**，未接入生产 UI/预处理链路；生产端到端拒绝行为留待阶段 B 实现后验证。通过校验不构成边界误差或质量保证。
- C3 查询规则为**参考策略测试**（合成解析信号 + 坐标恒等式），生产 kernel 的补偿查询实现留待阶段 B；本轮未修改任何生产代码。
- 长参考触发前模型、起端/尾端边界效应数值、合成干扰量化维持二轮 UNVERIFIED 表述；本轮未新增实机验证。
- exp3 T1–T7 数值链本轮未改动代码（仅头注/params 文本同步），其断言经统一入口复跑复核；两态 p′ 残差（1.1e-11 相对）仍为查询坐标双精度舍入（statePPNote 维持）。
- b 解析误差（σ=2 应力脉冲亚样本导数插值）维持仅报告不判定（R3 限制）。
- 权重/归一化选择仍是独立待审决策点（本轮未动）。

## 阶段 B/C

**仍未开始。** 本次仅触及 `CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/` 内参考/测试/证据/文档；是否 APPROVE 由规划主代理独立复审决定；本回执不自动授权阶段 B/C。

---

## 四轮收尾补测记录（任务 `环形成像阶段A最小收尾与方向校准_20260920-122834.md`，起点 77b1d37）

本节为该任务的证据交付部分；任务回执整体见 [stage-A-closeout.md](stage-A-closeout.md)。

### 补测脚本与统一入口

- **`matlab/test_endpoint_stability.m`【新·四轮】**——起端/近尾报告区间的长参考 pad 稳定性补测：两场景实际报告区间上，非参考 pad {2000,4000} vs 参考 pad 8000，走 padConvergenceCheck 统一判据（与 S3/exp5 B6 同一入口，容差 1e-9）；并锚定复算 77b1d37 已发布敏感性数值。唯一延长策略：若 {2000,4000} 不足则仅一次改用 {4000,8000} vs 参考 16000；仍失败则如实报告 FAIL，不放宽阈值、不以自比计入通过（本轮未触发延长）。
- **`matlab/run_all_remediation.m`**——统一入口注册一行（test_endpoint_stability.m，位于 test_query_policy 之后、exp1 之前）；其余 11 项脚本零改动。

### 两场景实测结果（evidence/test_endpoint_stability.json，MATLAB R2023a 9.14.0.2206163，退出码 0）

| 场景 | 实际取样区间（0 基 τ） | verdict | firstOkPad | nOk | maxPairRel（实际两两差） | relapse |
|---|---|---|---|---|---|---|
| startBurst（burstOnly，窗=MATLAB 1:1500） | 0..1499 | PASS | 2000 | 2 | 1.25e-13 | 无 |
| endEcho（echoOnly τ=3600，窗=MATLAB 3100:3643） | 3099..3642（线末截断） | PASS | 2000 | 2 | 9.38e-14 | 无 |

相对 pad=8000 参考的窗内幅度误差（容差 1e-9）：

| 场景 | pad=2000 | pad=4000 | 两候选窗差 p/p′/b |
|---|---|---|---|
| startBurst relP/relPP/relB | 1.3e-13 / 3.8e-14 / 6.0e-14 | 7.7e-14 / 3.3e-14 / 4.8e-14 | 1.25e-13 / 4.6e-14 / 6.0e-14 |
| endEcho relP/relPP/relB | 9.4e-14 / 1.5e-14 / 1.6e-14 | 0（与参考逐位一致） | 9.38e-14 / 1.5e-14 / 1.6e-14 |

绝对误差与尺度（pad=2000 行）：startBurst absP=2.2e-10 / absPP=1.0e-3 / absB=1.7e-9（scaleP=1712.28、scalePP=2.61e10、scaleB=2.80e4）；endEcho absP=4.6e-14 / absPP=2.4e-7 / absB=7.0e-12（scaleP=0.490、scalePP=1.57e7、scaleB=451.1）。全表见 JSON `attempts.absP/absPP/absB` 与 `attempts.scaleP/scalePP/scaleB`。

### 锚定复算（证明补测与原敏感性计算同构）

短窗 vs pad=8000 参考行的敏感性数值逐位复现 77b1d37 已发布值（相对差恰为 0）：

- startBurst |Δp|rel = 0.31117044891792733（已发布 0.31117044891792733）
- endEcho |Δp|rel = 0.63275363030561171、|Δb|rel = 0.13554445421030059（已发布同值）

原敏感性数值在稳定参考下**不变**：跨 pad 相对稳定度 startBurst p/p′/b = 3.4e-13 / 2.6e-14 / 2.5e-13；endEcho = 6.5e-15 / 6.7e-14 / 6.9e-14（短窗 vs 2000/4000/8000 三个参考的敏感性数值一致到 1e-13 量级）。

### 结论更正与保留

- **更正**：本文件上方"起端/尾端窗口稳定性核对"段（77b1d37 版）撤回；端点稳定性证据由本补测的实测非自比数据取代。8000 是参考 pad，不是非参考候选。
- **保留**：C1 判据本身、S3/S3n 全部证据（中心窗收敛与判据正负例的结论不变，只是不再被引用为端点证据）、起端 0.311/近尾 0.633 有限窗边界效应量化、实机 UNVERIFIED。
- **不要求**：短窗与长窗误差小或为零——有限窗边界效应是被测对象；短/长导数在窗边缘样本的端点约定差异（单侧 vs 中心差分，exp3 T7 既定规则）随数值记录、不判定。
- 本轮仅运行新增补测；C1 负例、C2/C3、R1/R2 代码零改动，未重跑（引用 77b1d37 已验证证据）。
