# 阶段 A 最小收尾回执：端点稳定性补测与方向校准

任务：`codex/task-docs:TASKS/环形成像阶段A最小收尾与方向校准_20260920-122834.md`（发布机器本地时间 2026-09-20 12:28:34；任务分支提交 `b702139`）
分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
starting SHA：`77b1d37e34dfbbf4b4f799328b8737a14f026339`（任务指定起点；fetch 后 `git rev-parse HEAD` 核对一致，工作区干净）
final SHA：见本提交；推送后 local HEAD == remote HEAD == ls-remote 三处核对结果见下方"推送回执"。
环境：MATLAB R2023a（9.14.0.2206163）+ Signal Processing Toolbox，Windows。
提交方式：新增提交（未 amend/rebase/squash/force push），未合并 main，未开始 B/C，未修改生产源码/UI/ABI，未构建 DLL/运行包（全部改动限于 `CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/`）。
上位任务链：`环形成像零相位滤波与光声反演滤波_20260919-024359.md` → R1–R5（[review-remediation.md](review-remediation.md)）→ B1–B3（[review-spec-closure.md](review-spec-closure.md)）→ C1/C2/C3（[review-final-closure.md](review-final-closure.md)）→ 本回执。
生产基线（未合并、未动）：`fb10721e07f9ea8c9e46bc308d250b788defb829`。

---

## 1. 端点稳定性补测（任务 §7.A 唯一证据缺口，已完成）

**缺口**：review-final-closure.md 初版（77b1d37）"起端/尾端窗口稳定性核对"段声称端点已由不同长窗口确认，但实际收敛计算在目标 τ=800 的中心区间 [300,1300]，起端 burst 与近尾 τ=3600 敏感性只用 padRef=8000 单一长参考；该段把中心收敛与人工正例当端点证据，且把 8000 写成非参考 pad。**该段已就地撤回**（四轮注记 + 原文删除线保留，原文见 git 历史 77b1d37），由本补测的实测非自比证据取代。

**补测脚本**：`matlab/test_endpoint_stability.m`【新】（统一入口 `run_all_remediation.m` 注册一行，四轮 12 项；其余 11 项脚本零改动）。

**两场景实测**（`evidence/test_endpoint_stability.json`；实际取样区间显式记录，非自比——参考 pad 8000 自比行不参与判定）：

| 场景 | 实际取样区间（0 基声学 τ） | 非参考 pad | 参考 pad | verdict | firstOkPad | maxPairRel | 敏感性数值锚定（短窗 vs 8000 行 vs 77b1d37 已发布值） |
|---|---|---|---|---|---|---|---|
| startBurst（burstOnly，原报告区间 MATLAB 1:1500） | τ=0..1499 | 2000、4000 | 8000 | **PASS** | 2000 | 1.25e-13 | \|Δp\|rel=0.31117044891792733，相对差 **0** |
| endEcho（echoOnly τ=3600，原敏感性实际计算区间 MATLAB 3100:3643） | τ=3099..3642（上限被短窗线末截断，不是未截断的 [3100,4100]） | 2000、4000 | 8000 | **PASS** | 2000 | 9.38e-14 | \|Δp\|rel=0.63275363030561171、\|Δb\|rel=0.13554445421030059，相对差 **0** |

- 判据：`padConvergenceCheck` 统一入口（容差 1e-9，与 S3/exp5 B6 同一标准）：≥2 非参考 pad 通过、两两稳定（实际两两差）、无尾段回落——两场景均满足，**未触发延长策略**（pad {2000,4000} 一次通过；预案为唯一一次延长 {4000,8000} vs 参考 16000，未使用）。
- p/p′/b 相对参考误差：startBurst pad2000 relP/relPP/relB = 1.3e-13/3.8e-14/6.0e-14、pad4000 = 7.7e-14/3.3e-14/4.8e-14；endEcho pad2000 = 9.4e-14/1.5e-14/1.6e-14、pad4000 = 0（与参考逐位一致）。绝对误差与尺度全表见 JSON（startBurst scaleP=1712.28/scalePP=2.61e10/scaleB=2.80e4；endEcho scaleP=0.490/scalePP=1.57e7/scaleB=451.1）。
- **原敏感性数值在稳定参考下不变**：跨 pad 相对稳定度 p/p′/b = startBurst 3.4e-13/2.6e-14/2.5e-13、endEcho 6.5e-15/6.7e-14/6.9e-14；77b1d37 的起端 0.311/近尾 0.633 量化表述**维持**（补测检验的是长参考稳定性，不要求短窗与长窗误差小或为零——有限窗边界效应是被测对象）。实机 UNVERIFIED 维持。

## 2. 三项方向校准（任务 §7.B，已写入 algorithm-stage-A.md §10 与本回执）

1. **反演定位**（§10.1）：候选 `b=2(p−t·p′)` 是复用现有 DAS 反投影框架的**可关闭工程近似增强**——单环应用与双声速直线走时不声称严格二维 FBP、非均匀精确反演或已证明实际图像全面改善；测试充分性仅限明确覆盖条件。
2. **权重选择依据与阶段 B 最小实现优先级**（§10.2）：**撤回**"只换信号必然严重劣化、因此必须换新权重"的强因果（其初版证据已作废，公平对照互有胜负维持 §4.3）。阶段 B 起草优先级：先评估保留旧固定 q=1 + accW、仅加反演信号的最小候选（legacyW 对照已有），立体角权重留离线比较依据；本轮不重跑权重实验、不新增可调权重 UI。该优先级不是本任务修改生产公式的授权，legacyW 不称为严格 UBP；最终生产权重/归一化由阶段 B 任务明确，阶段 A 不以"证明某权重全面占优"为退出条件。
3. **DBR 限制定位**（§10.3）：C2 拒绝规则按已发布规格保留、不放开不扩大；文案定位为"本版未支持/未验证的组合"，不是数学禁忌；通过配置校验不保证端点无失真；起端 0.311/近尾 0.633 与实机 UNVERIFIED 维持。

## 3. 文档修订位置

| 文件 | 修订 |
|---|---|
| `review-final-closure.md` | "起端/尾端窗口稳定性核对"段四轮注记撤回（含撤回原因：中心窗证据不覆盖端点、[3100,4100] 未截断区间与实际 3100:3643 不符、8000 误写为非参考 pad）；文末新增"四轮收尾补测记录"节（脚本/入口/两场景表/锚定复算/结论更正与保留） |
| `algorithm-stage-A.md` | 头部四轮任务行与基线链（77b1d37 为四轮起点）；§6 汇总表新增 test_endpoint_stability 行与统一入口 12 项；§9 审查清单第 12 条；新增 §10"阶段 A 方向校准与收尾"（§10.1 反演定位 / §10.2 权重选择依据与阶段 B 最小实现优先级 / §10.3 DBR 限制定位 / §10.4 阶段 B/C 入口） |
| `README.md` | 四轮任务行、状态更新（参考准备完成待最终审查；真实收益/性能/UI/生产实现未完成）、内容清单（test_endpoint_stability.m、stage-A-closeout.md）、实验索引新行、复跑条目与 B/C 边界说明 |
| `matlab/run_all_remediation.m` | 统一入口注册一行（test_endpoint_stability.m，12 项） |
| `matlab/test_endpoint_stability.m` | 【新】补测脚本（见 §1） |
| `evidence/test_endpoint_stability.json` | 【新】补测证据 |

## 4. 实际运行命令与退出码

```
工作目录：CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/matlab
命令：matlab -batch "run('test_endpoint_stability.m')"
MATLAB R2023a 9.14.0.2206163（Windows）；退出码 0；断言 E1–E6 全部通过：
  E1_startRefStable=1 E3_anchorStartReproduced=1 E5_startSensStable=1
  E2_endRefStable=1  E4_anchorEndReproduced=1  E6_endSensStable=1
```

`matlab -batch "run_all_remediation"` 本轮未整体重跑（其余 11 项代码零改动，按任务 §10 引用 77b1d37 已验证证据；统一入口注册行经 checkcode 复核，与 HEAD 相比无新增告警）。

## 5. 引用但未重跑的旧证据（77b1d37）

- C1 判据负例/正例（S3n 9 例）、中心窗 pad 收敛重算（S3/exp5 B6）、C2 配置矩阵 80 用例、C3 查询规则 Q1–Q6、R1/R2（test_time_derivative/test_fair_comparison）、exp1–exp6 全部数值链——代码零改动，本轮未重跑，引用 77b1d37 已验证证据（11/11 统一复跑通过记录见 review-final-closure.md"复跑命令回执"）。

## 6. 未验证项（维持）

- 真实成像质量、真实数据收益、运行节拍/性能、硬件行为、UI 与生产实现：**UNVERIFIED / 未完成**，属阶段 B/C。
- 长参考触发前模型（恒 0 合成约定）实机可得性 UNVERIFIED；端点补测结论限于该合成模型。
- b 亚样本导数插值误差（R3 限制）、权重/归一化待审决策点（§4.3/§10.2）：维持原文。
- 阶段 A 收尾不自动解锁 B/C；本回执不代替 B/C 结论。

## 7. 推送回执

推送命令（任务 §14）：

```powershell
git push origin HEAD:refs/heads/codex/ring-zero-phase-pa-inversion-20260919-025754
git fetch origin refs/heads/codex/ring-zero-phase-pa-inversion-20260919-025754:refs/remotes/origin/codex/ring-zero-phase-pa-inversion-20260919-025754
git rev-parse HEAD
git rev-parse origin/codex/ring-zero-phase-pa-inversion-20260919-025754
git ls-remote origin refs/heads/codex/ring-zero-phase-pa-inversion-20260919-025754
```

三处 SHA 一致核对结果：见本回执提交后的推送命令实际输出（执行代理在任务返回中报告 local HEAD == remote-tracking == ls-remote 三处一致的 final SHA）。未合并 main。
