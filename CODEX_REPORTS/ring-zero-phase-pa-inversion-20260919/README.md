# ring-zero-phase-pa-inversion-20260919 — 阶段 A 交付（审查整改版）

任务：`codex/task-docs:TASKS/环形成像零相位滤波与光声反演滤波_20260919-024359.md`（远端已推送，提交 `7236c03a69b2144e7307cd5135fe441fa6fd8cc4`）
一轮整改依据：`TASKS/环形成像阶段A审查整改追加_20260919-201935.md`（R1–R5；逐项回应见 [review-remediation.md](review-remediation.md)）
二轮整改依据：`TASKS/环形成像阶段A边界参考与时间轴规格收口_20260919-235753.md`（B1–B3；逐项回应见 [review-spec-closure.md](review-spec-closure.md)）

实现分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
（自 origin/main `fb10721e07f9ea8c9e46bc308d250b788defb829` 创建；一轮审查基线 `94939627f48ccc95f60c4995c99be4e371aaa2ca`，二轮起点 `9ace9e10f0886512954dac6f69ce01e715a4591e`；整改均以新增提交完成，未 amend/rebase）

## 状态

**阶段 A 二次整改完成，待规划主代理重新审查。B/C 未开始。整项功能未完成。**
两轮仅修改阶段 A 参考/测试/证据/文档，无生产实现改动。

## 内容

- `algorithm-stage-A.md` —— 算法说明（二轮统一时间轴记号 q_raw=s+D−1、独立长参考边界证据、撤回项清单）
- `matlab/exp1..exp6` —— 判定性与参考实验（exp2 B3 打包修正、exp3 新增 T7/p′ 字段、exp5 B6 重写；exp1/exp4/exp6 未改码重跑复核）
- `matlab/longReference.m` —— 【新·二轮】独立长窗边界参考（扩展区间整条滤波后按物理时间提取）
- `matlab/ringAcousticModel.m` —— 【新·二轮】共享声学信号模型（短窗/长窗逐位一致的前提）
- `matlab/timeDerivative.m` —— dim1 时间导数唯一入口（R1 修复）
- `matlab/reconPairKernels.m` —— 权重/归一化对照共享核（R2 修复）
- `matlab/test_time_derivative.m`、`test_fair_comparison.m` —— R1/R2 自动测试
- `matlab/test_boundary_reference.m` —— 【新·二轮】B1 结构/负例/pad 收敛/对齐/敏感性测试（失败即非零退出）
- `matlab/run_all_remediation.m` —— 统一复跑入口（任一断言失败 → 非零退出）
- `matlab/refZeroPhase.m` —— 零相位滤波共享参考实现（未改动）
- `evidence/exp*.json`、`exp*_*.mat`、`filter_reference_vectors.mat`、`test_*.json` —— 数值证据（受影响者已全部重生成）
- `review-remediation.md` —— R1–R5 整改回应（二轮冲突项已就地标注修正）
- `review-spec-closure.md` —— 【新·二轮】B1–B3 整改回应、撤回项清单与命令回执

## 实验索引（二轮整改后）

| 脚本 | 判定内容 | 结果 |
|---|---|---|
| test_time_derivative.m | R1：导数维度/离散约定四项断言、路径一致、导出向量核查 | 全部 PASS |
| test_fair_comparison.m | R2：显式归一化、权重族 accW 一致、非符号图、形状相关 0.939 | 全部 PASS |
| test_boundary_reference.m | 二轮 B1：长参考结构、旧自比构造负例、pad 收敛（1e-9）、物理对齐、起/终端敏感性可测性 | 全部 PASS |
| exp1_ubp_sphere.m | Xu-Wang PRE2005 UBP 常数（紧支撑均匀球） | 球内误差 6.4e-8（复核；球外残差措辞已收敛） |
| exp2_ring_ubp_2d.m | 2D 环形五模式因子分解（信号×权重×归一化） | 盘 CV：das 0.0153 / legacyW 0.0334 / ubpD 0.0080 / ubpP 0.1084 / saP 0.0874；B3 修复后数值逐位不变 |
| exp3_time_axis.m | 时间轴/脉冲测试 + delayCut 两态完整反演等价 + T7 恒等式/线端探针 | 两态差 ≤3.1e-11（b）/ 1.1e-11 相对（p′）；T7 恒等式 4.2e-22；生产 delayCut=0 旧行为偏差 0.985（集成建议见 §2.3） |
| exp4_dual_layer.m | 分层走时一致性/同速退化 | 3.2e-17m；4.5e-13 样本；0.000mm（复核） |
| exp5_order_endpoints.m | 滤波顺序/实现归因/DBR 边界矩阵/近边界目标（独立长参考） | 顺序效应 0（无干扰）；近边界无干扰 \|Δp\|rel ≤2.1e-9、burst 起点 0.311、近尾 0.633（旧"精确 0"自比结论已撤回）；dbrEnd≥sysDelay 拒绝策略（B2 证据） |
| exp6_filter_reference.m | SOS/零相位参考/频响/记忆长度/向量导出 | 全部断言 PASS（复核；导出 p′/b 未受 R1 影响） |

## 复跑方式

统一入口（工作目录 = `matlab/`）：

```powershell
cd CODEX_REPORTS\ring-zero-phase-pa-inversion-20260919\matlab
matlab -batch "run_all_remediation"
```

单个测试/实验：

```powershell
matlab -batch "run('test_boundary_reference.m')"
matlab -batch "run('exp2_ring_ubp_2d.m')"
```

任一关键断言失败均使运行非零退出。二轮全量复跑 9/9 项 OK、退出码 0（MATLAB R2023a，回执见 review-spec-closure.md）。

## 阶段 B/C

未开始。阶段 A 是否 APPROVE 由规划主代理独立复审决定；整改提交不自动解锁 B/C。
