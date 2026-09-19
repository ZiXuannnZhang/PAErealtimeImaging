# ring-zero-phase-pa-inversion-20260919 — 阶段 A 交付

任务：`codex/task-docs:TASKS/环形成像零相位滤波与光声反演滤波_20260919-024359.md`
（注意：该任务文档当前位于本地 `codex/task-docs` 未推送提交 `7236c03`，远端 `origin/codex/task-docs` 尚无此文件——已向规划主代理报告。）

实现分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`（自 origin/main `fb10721e07f9ea8c9e46bc308d250b788defb829` 创建）

## 阶段 A 内容（本次提交）

- `algorithm-stage-A.md` —— 算法说明（反演依据/时间轴/顺序端点/权重归一化/分层声速/容差/审查要点）
- `matlab/exp1..exp6` —— 判定性与参考实验（MATLAB R2023a + Signal Processing Toolbox）
- `matlab/refZeroPhase.m` —— 零相位滤波共享参考实现（阶段 B C++ 的对照算法）
- `evidence/exp*.json`、`exp*_*.mat`、`filter_reference_vectors.mat` —— 数值证据与阶段 B 参考向量

## 实验索引

| 脚本 | 判定内容 | 结果 |
|---|---|---|
| exp1_ubp_sphere.m | Xu-Wang PRE2005 UBP 常数（紧支撑均匀球体） | 球内误差 6.4e-8 |
| exp2_ring_ubp_2d.m | 2D 环形 DAS vs UBP（P/D/wrong 配对） | accW 归一化最优；错误配对劣化 |
| exp3_time_axis.m | delayCut 两态索引/脉冲测试 | 亚样本对齐；±1 样本可分辨 |
| exp4_dual_layer.m | 分层走时一致性/同速退化 | 3.2e-17m；4.5e-13 样本；0.000mm |
| exp5_order_endpoints.m | 滤波顺序/端点/启动强信号 | 提议顺序不劣于历史切片 |
| exp6_filter_reference.m | SOS/零相位参考/频响/记忆长度/向量导出 | 全部断言 PASS |

复跑方式（任一 exp）：`matlab -batch "run('expN_xxx.m')"`（在 `matlab/` 目录下）。

## 阶段 B/C

未开始。待阶段 A 审查（规划主代理数学/时间轴审查）通过后，按 `algorithm-stage-A.md` §7 要点在本分支继续。
