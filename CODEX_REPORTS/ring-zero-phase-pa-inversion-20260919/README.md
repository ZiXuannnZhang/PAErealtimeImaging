# ring-zero-phase-pa-inversion-20260919 — 阶段 A 交付（审查整改版）

任务：`codex/task-docs:TASKS/环形成像零相位滤波与光声反演滤波_20260919-024359.md`（远端已推送，提交 `7236c03a69b2144e7307cd5135fe441fa6fd8cc4`）
一轮整改依据：`TASKS/环形成像阶段A审查整改追加_20260919-201935.md`（R1–R5；逐项回应见 [review-remediation.md](review-remediation.md)）
二轮整改依据：`TASKS/环形成像阶段A边界参考与时间轴规格收口_20260919-235753.md`（B1–B3；逐项回应见 [review-spec-closure.md](review-spec-closure.md)）
三轮整改依据：`TASKS/环形成像阶段A收敛判据与配置规则最终收口_20260920-101451.md`（C1/C2/C3；逐项回应见 [review-final-closure.md](review-final-closure.md)）
四轮收尾依据：`TASKS/环形成像阶段A最小收尾与方向校准_20260920-122834.md`（端点稳定性补测 + 三项方向校准；回执见 [stage-A-closeout.md](stage-A-closeout.md)）

实现分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
（自 origin/main `fb10721e07f9ea8c9e46bc308d250b788defb829` 创建；一轮审查基线 `94939627f48ccc95f60c4995c99be4e371aaa2ca`，二轮起点 `9ace9e10f0886512954dac6f69ce01e715a4591e`，三轮起点 `63e088a25a67e941d710be79ca501b3904801bc7`，四轮起点 `77b1d37e34dfbbf4b4f799328b8737a14f026339`；整改均以新增提交完成，未 amend/rebase）

## 状态

**阶段 A 四轮最小收尾完成（端点稳定性补测 + 方向校准），参考准备完成待最终审查。B/C 未开始。真实图像收益/性能/UI/生产实现未完成。**
四轮仅修改阶段 A 参考/测试/证据/文档（新增 test_endpoint_stability.m 与证据、review-final-closure.md 端点证据归属段撤回更正、algorithm-stage-A.md §10 方向校准），无生产实现改动。

## 内容

- `algorithm-stage-A.md` —— 算法说明（三轮同步：C1 收敛判据、C2 配置冻结规则、C3 唯一查询规则；四轮新增 §10 方向校准：反演定位/权重选择依据与阶段 B 最小实现优先级/DBR 限制定位）
- `matlab/exp1..exp6` —— 判定性与参考实验（exp3/exp5 头注与 policy 字段三轮同步；exp1/exp4/exp6 未改码）
- `matlab/longReference.m` —— 【新·二轮】独立长窗边界参考（扩展区间整条滤波后按物理时间提取）
- `matlab/ringAcousticModel.m` —— 【新·二轮】共享声学信号模型（短窗/长窗逐位一致的前提）
- `matlab/padConvergenceCheck.m` —— 【新·三轮 C1】pad 收敛判据唯一入口（padRef 自比不计入、≥2 非参考 pad、两两稳定、无再次超差、负例全 FAIL）
- `matlab/filterDbrConfigCheck.m` —— 【新·三轮 C2】滤波×DBR×delayCut 配置前置校验参考实现（不接入生产 UI）
- `matlab/timeDerivative.m` —— dim1 时间导数唯一入口（R1 修复）
- `matlab/reconPairKernels.m` —— 权重/归一化对照共享核（R2 修复）
- `matlab/test_time_derivative.m`、`test_fair_comparison.m` —— R1/R2 自动测试
- `matlab/test_boundary_reference.m` —— B1 结构/负例/pad 收敛/对齐/敏感性测试 + C1 判据正负例（S3n）
- `matlab/test_filter_dbr_config.m` —— 【新·三轮 C2】配置矩阵测试（80 用例：50 接受/30 拒绝）
- `matlab/test_query_policy.m` —— 【新·三轮 C3】三组合查询规则策略测试（Q1–Q6）
- `matlab/test_endpoint_stability.m` —— 【新·四轮】起端/近尾报告区间长参考 pad 稳定性补测（实际取样区间 + 77b1d37 敏感性数值锚定复算）
- `matlab/run_all_remediation.m` —— 统一复跑入口（任一断言失败 → 非零退出；四轮 12 项）
- `matlab/refZeroPhase.m` —— 零相位滤波共享参考实现（未改动）
- `evidence/exp*.json`、`exp*_*.mat`、`filter_reference_vectors.mat`、`test_*.json` —— 数值证据（受影响者已全部重生成）
- `review-remediation.md` —— R1–R5 整改回应（二/三轮冲突项已就地标注修正）
- `review-spec-closure.md` —— B1–B3 整改回应（三轮冲突项已就地标注修正）
- `review-final-closure.md` —— 【新·三轮】C1/C2/C3 整改回应、撤回项清单与命令回执（四轮：端点证据归属段就地撤回 + 四轮补测记录节）
- `stage-A-closeout.md` —— 【新·四轮】阶段 A 最小收尾回执（补测结果、三项方向校准、命令与 SHA 回执）

## 实验索引（四轮收尾后）

| 脚本 | 判定内容 | 结果 |
|---|---|---|
| test_time_derivative.m | R1：导数维度/离散约定四项断言、路径一致、导出向量核查 | 全部 PASS |
| test_fair_comparison.m | R2：显式归一化、权重族 accW 一致、非符号图、形状相关 0.939 | 全部 PASS |
| test_boundary_reference.m | B1+C1：长参考结构、旧自比构造负例、pad 收敛（统一判据+人工误差表正负例 S3n）、物理对齐、起/终端敏感性可测性 | 全部 PASS（收敛容差 1e-9；判据负例 6 必败 + 正例 3 必过） |
| test_filter_dbr_config.m | C2：滤波×DBR×delayCut 配置矩阵、逐通道拒绝、E 真实语义、旧行为保持、非法参数报错 | 80 用例全 PASS（50 接受/30 拒绝按冻结规则表） |
| test_query_policy.m | C3：三组合查询规则、未裁剪偏移恰 D−1、分层后偏移、乘子恒等、选择唯一、两态同位 | Q1–Q6 全 PASS（偏移 1.14e-13 浮点级） |
| test_endpoint_stability.m | 四轮：起端（τ=0..1499）/近尾（τ=3099..3642）报告区间长参考 pad 稳定性（{2000,4000} vs 参考 8000）+ 敏感性数值锚定复算 | 两场景 PASS（maxPairRel 1.25e-13/9.38e-14；0.311/0.633/0.1355 逐位复现） |
| exp1_ubp_sphere.m | Xu-Wang PRE2005 UBP 常数（紧支撑均匀球） | 球内误差 6.4e-8（复核；球外残差措辞已收敛） |
| exp2_ring_ubp_2d.m | 2D 环形五模式因子分解（信号×权重×归一化） | 盘 CV：das 0.0153 / legacyW 0.0334 / ubpD 0.0080 / ubpP 0.1084 / saP 0.0874 |
| exp3_time_axis.m | 时间轴/脉冲测试 + delayCut 两态完整反演等价 + T7 恒等式/线端探针（头注 C3 同步） | 两态差 ≤3.1e-11（b）；T7 恒等式 4.2e-22；不补偿偏差 0.985 |
| exp4_dual_layer.m | 分层走时一致性/同速退化 | 3.2e-17m；4.5e-13 样本；0.000mm（复核） |
| exp5_order_endpoints.m | 滤波顺序/实现归因/DBR 边界矩阵/近边界目标（收敛判据 C1 版；policy C2/C3 同步） | 顺序效应 0（无干扰）；近边界无干扰 \|Δp\|rel ≤2.1e-9、burst 起点 0.311、近尾 0.633 |
| exp6_filter_reference.m | SOS/零相位参考/频响/记忆长度/向量导出 | 全部断言 PASS（复核） |

## 复跑方式

统一入口（工作目录 = `matlab/`）：

```powershell
cd CODEX_REPORTS\ring-zero-phase-pa-inversion-20260919\matlab
matlab -batch "run_all_remediation"
```

单个测试/实验：

```powershell
matlab -batch "run('test_boundary_reference.m')"
matlab -batch "run('test_filter_dbr_config.m')"
matlab -batch "run('test_query_policy.m')"
matlab -batch "run('test_endpoint_stability.m')"
```

任一关键断言失败均使运行非零退出。三轮收口复跑 11 项、退出码 0；四轮补测单跑退出码 0（MATLAB R2023a，回执见 review-final-closure.md 与 stage-A-closeout.md）。

## 阶段 B/C

未开始。阶段 A 是否 APPROVE 由规划主代理独立复审决定；整改提交不自动解锁 B/C。阶段 A 参考准备完成，不代替 B/C 结论（真实图像收益未证明、性能未测、生产实现未动）。
