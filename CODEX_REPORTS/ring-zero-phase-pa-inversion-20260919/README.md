# ring-zero-phase-pa-inversion-20260919 — 阶段 A 交付（审查整改版）

任务：`codex/task-docs:TASKS/环形成像零相位滤波与光声反演滤波_20260919-024359.md`（远端已推送，提交 `7236c03a69b2144e7307cd5135fe441fa6fd8cc4`）
整改依据：`TASKS/环形成像阶段A审查整改追加_20260919-201935.md`（R1–R5；逐项回应见 [review-remediation.md](review-remediation.md)）

实现分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
（自 origin/main `fb10721e07f9ea8c9e46bc308d250b788defb829` 创建；整改前审查基线 `94939627f48ccc95f60c4995c99be4e371aaa2ca`，整改以新增提交完成，未 amend/rebase）

## 状态

**阶段 A 整改完成，待规划主代理重新审查。B/C 未开始。整项功能未完成。**
本轮仅修改阶段 A 参考/测试/证据/文档，无生产实现改动。

## 内容

- `algorithm-stage-A.md` —— 算法说明（整改后全文重算/修订；含两态查询公式、DBR 边界拟定策略、权重族标签勘误、撤回项清单）
- `matlab/exp1..exp6` —— 判定性与参考实验（exp2/exp3/exp5 整改重写；exp1/exp4/exp6 未改码重跑复核）
- `matlab/timeDerivative.m` —— 【新】dim1 时间导数唯一入口（R1 修复）
- `matlab/reconPairKernels.m` —— 【新】权重/归一化对照共享核（R2 修复）
- `matlab/test_time_derivative.m`、`test_fair_comparison.m` —— 【新】R1/R2 自动测试（失败即非零退出）
- `matlab/run_all_remediation.m` —— 【新】统一复跑入口（任一断言失败 → 非零退出）
- `matlab/refZeroPhase.m` —— 零相位滤波共享参考实现（未改动）
- `evidence/exp*.json`、`exp*_*.mat`、`filter_reference_vectors.mat`、`test_*.json` —— 数值证据（受影响者已全部重生成）
- `review-remediation.md` —— 【新】R1–R5 逐项整改回应、命令回执与结论变化

## 实验索引（整改后）

| 脚本 | 判定内容 | 结果（整改后） |
|---|---|---|
| test_time_derivative.m | R1：导数维度/离散约定四项断言、路径一致、导出向量核查 | 全部 PASS |
| test_fair_comparison.m | R2：显式归一化、权重族 accW 一致、非符号图、形状相关 0.939 | 全部 PASS |
| exp1_ubp_sphere.m | Xu-Wang PRE2005 UBP 常数（紧支撑均匀球） | 球内误差 6.4e-8（复核；球外残差措辞已收敛） |
| exp2_ring_ubp_2d.m | 2D 环形五模式因子分解（信号×权重×归一化） | 盘 CV：das 0.0153 / legacyW 0.0334 / ubpD 0.0080 / ubpP 0.1084 / saP 0.0874；旧"wrong"结论撤回 |
| exp3_time_axis.m | 时间轴/脉冲测试 + delayCut 两态完整反演等价 | 两态差 ≤3.1e-11；跨通道 0；生产 delayCut=0 旧行为偏差 0.985（阶段 B 适配项） |
| exp4_dual_layer.m | 分层走时一致性/同速退化 | 3.2e-17m；4.5e-13 样本；0.000mm（复核） |
| exp5_order_endpoints.m | 滤波顺序/实现归因/DBR 边界矩阵/近边界目标 | 顺序效应 0（无干扰）；边界约定误差精确 0；dbrEnd≥sysDelay 拒绝策略（B2 证据） |
| exp6_filter_reference.m | SOS/零相位参考/频响/记忆长度/向量导出 | 全部断言 PASS（复核；导出 p′/b 未受 R1 影响） |

## 复跑方式

统一入口（工作目录 = `matlab/`）：

```powershell
cd CODEX_REPORTS\ring-zero-phase-pa-inversion-20260919\matlab
matlab -batch "run_all_remediation"
```

单个测试/实验：

```powershell
matlab -batch "run('test_time_derivative.m')"
matlab -batch "run('exp2_ring_ubp_2d.m')"
```

任一关键断言失败均使运行非零退出。本轮全量复跑退出码 0（MATLAB R2023a，回执见 review-remediation.md）。

## 阶段 B/C

未开始。阶段 A 是否 APPROVE 由规划主代理独立复审决定；整改提交不自动解锁 B/C。
