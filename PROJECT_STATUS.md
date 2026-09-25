# PAERealtimeImaging 当前项目状态

> 更新时间：2026-09-25（UTC+8）
>
> 本文件是**当前项目状态的单一事实入口**。分支/历史治理以 `REPOSITORY_BASELINE.md` 为准；构建与交付以 `BUILD_STANDARD.md` 为准；具体任务特殊要求以 `codex/task-docs:TASKS/<task>.md` 为准。

## 1. 当前结论

- `main` 是唯一 canonical branch。
- Session A/B/C/D 工作链已完成软件实现、独立审查、自动化回归、Windows 构建、Ring/CUDA 软件自检和候选交付整理。
- 已接受的 A/B/C/D candidate production/test source identity 为：
  `d9daa2d7af6bb8341349e405433824e89e42bcd4`。
- Session D final traceability HEAD 为：
  `69a7606f95c97c839fd618115f4092a4291d8906`。
- 用户于 2026-09-18 反馈：**当前实机验证范围内暂未发现与预期不符合的行为，初步认为本轮工作完成。**
- 因此本轮正式状态定级为：
  `A/B/C/D_FUNCTIONAL_HARDWARE_VALIDATION = PASS_FOR_CURRENT_SCOPE`。
- 但现场额外 startup trigger 的**底层 FPGA/LabVIEW 精确来源仍未证明**；“7 / 4007”继续只是当前控制环境中的现场观察/操作配置，不是协议常量。
- START-admission 软件修复已经存在于本次接受的 source tree 中；这不等于已证明历史启动 ingress-loss 的唯一硬件根因。
- 远端旧实现/验证分支采用**非破坏性整理**：保留用于 traceability，不删除、不重写，不再作为新任务默认 baseline。
- **2026-09-24**：已通过实机验收的前端链（全分辨率显示裁切 → Frontend Preprocessing Stage → 逐 A-line 零相位前端滤波 → 保存/闸门与写盘故障修复 → 显示命名）以 **fast-forward** 进入 canonical `main`：无 merge commit、无 force push、无 history 重写。canonical `main` = `491aa34cfa9553954eea949a7703df7194eb7699`。
- **2026-09-24**：环形成像零相位滤波与光声反演分支 `codex/ring-zero-phase-pa-inversion-20260919-025754` 定为**只作参考、不再实现或维护**（详见第 6 节）。其阶段 B1 任务中「B1 通过后的 exact SHA 将作为 B2 起点」的约定**作废**：后续环形成像工作一律从 latest `origin/main` 出发。
- **2026-09-25**：DAS 双波长质量增强 B 档分支 `codex/das-dual-wavelength-quality-b-tier-20260925` 定为**实验性分支、后续不再维护、不在其上继续实现**（详见第 6.3 节）。该分支的生产/测试源改动**均未进入** canonical `main`；后续相关工作一律从 latest `origin/main` 出发，需要其中任何产物时须在 `main` 上重新落地或显式迁移。

当前状态标签：

```text
SESSION_A_SOFTWARE_REVIEW                 = APPROVE
SESSION_B_SOFTWARE_REVIEW                 = APPROVE
SESSION_C_SOFTWARE_REVIEW                 = APPROVE
SESSION_D_INTEGRATION_REVIEW              = APPROVE
SESSION_D_DELIVERY_CANDIDATE              = READY

A_B_C_D_FUNCTIONAL_HARDWARE_VALIDATION    = PASS_FOR_CURRENT_SCOPE
FPGA_LABVIEW_EXTRA_TRIGGER_EXACT_SOURCE   = NOT_PROVEN
FULL_ROUND_TRIGGER_4007                    = FIELD_OBSERVATION_NOT_PROTOCOL_CONSTANT

START_ADMISSION_SOFTWARE                  = INCLUDED_AND_APPROVED
START_ADMISSION_HARDWARE_ROOT_CAUSE       = NOT_PROVEN

CANONICAL_SOURCE_BASELINE                 = main
CANONICAL_SOURCE_IDENTITY                 = 491aa34cfa9553954eea949a7703df7194eb7699（源树身份）
CANONICAL_MAIN_HEAD                       = 以最新 origin/main 为准（其后仅文档提交推进 HEAD）
REMOTE_BRANCH_CLEANUP                     = NON_DESTRUCTIVE
RING_ZERO_PHASE_PA_INVERSION_BRANCH       = REFERENCE_ONLY_NO_MAINTENANCE
DAS_DUAL_WAVELENGTH_B_TIER_BRANCH         = EXPERIMENTAL_NO_FURTHER_WORK
NEXT_RING_WORK_BASELINE                   = latest origin/main（B1 起点契约作废）
```

## 2. Canonical source / accepted provenance

正式源码基线为当前 `main`。

本轮已接受实现链的关键追溯点：

```text
START admission accepted software point:
  6313540f72544c0f68820c4815903abaa0b8c1e1

PhysicalRound retained integrated point:
  52cf7713d7e0e935cb14663ec3470f3a25bfeb90

Session A final:
  eb283f64574d9043f4d4823338c31767c3b811fe

Session B final:
  676df4a946fed56b903474354984bff44ab50875

Session C candidate code / accepted production-test tree:
  d9daa2d7af6bb8341349e405433824e89e42bcd4

Session D final traceability HEAD:
  69a7606f95c97c839fd618115f4092a4291d8906

--- 2026-09-24 并入 canonical main 的前端链（已过实机验收）---

full-resolution display crop:
  c31fd09

Frontend Preprocessing Stage (Task 1 identity):
  5af8c39

per-A-line zero-phase frontend filtering:
  093f8cc

save/gate bugfix（落盘数据永不被改写）:
  4660740

write-fault rollback（H2/H1）:
  bf5c226

card-status / channel display naming:
  8f9e9cd

canonical main fast-forward HEAD:
  491aa34cfa9553954eea949a7703df7194eb7699

--- 2026-09-24 之后 main 的移动（仅文档提交，源码树未变）---

65de782  远端分支盘点与治理记录（PROJECT_STATUS.md / REPOSITORY_BASELINE.md）
f304242  分支角色标注（PROJECT_STATUS.md）

以上两笔不改任何 production/test source；main 的源码树身份仍为 491aa34。
canonical main HEAD 会继续因文档提交而移动，接手时一律以最新 origin/main 为准。
```

A/B/C/D 最终 candidate ancestry 中包含 START-admission 软件修复；不要在后续整理中手术式剥离该祖先，否则会形成未经同等验证的新 source tree。

## 3. 本轮冻结行为

### 3.1 Physical round policy

- `startupFilterTriggerCount=X` 以 **new distinct physical trigger identity** 为单位过滤前 X 枚。
- `disableCountBoundary=false`：configured logical count 保持 CountBoundary/final 语义。
- `disableCountBoundary=true`：configured count 仅作为 realtime imaging cap；raw/save 继续直到 timeout。
- `RoundIdentity=(measurementSession, roundGeneration)` 是跨 Normalizer/Ring/ImagingSvc/UI 的 ownership key。
- active physical-idle timeout 由 20ms PAimage poll 链单一负责；partial-startup round 也可 timeout。

### 3.2 Observability / save / reset

- `缺失=triggersPartial`，`跳号数=missingTriggerCount`，`丢包=packetsDropped`。
- production full-trigger gap owner 是 SourceCore -> `Decision::TriggerGap` -> CardStats。
- same-session 大幅 triggerSeq reset recovery 阈值为 256；small backstep 不 re-anchor。
- TimeoutBoundary 同步提交新 AutoSave binding，旧 raw write 保持旧 generation。
- old-round presentation capture 在 Ring/CUDA reset 前完成；disk write 可异步。
- timeout residual 不生成 synthetic partial Ring block，不改 `expectedBlocks`，不强制 completion。

## 4. 实机验证边界

当前用户反馈支持：

```text
当前验证范围功能行为 = PASS
未观察到 A/B/C/D 与预期不符合之处
```

仍不能据此声称：

```text
“7 个额外 startup trigger 的 FPGA/LabVIEW 精确产生机制已证明”
“4007 是协议规定的固定物理触发数”
“历史 START ingress-loss 的唯一根因已由 START-admission fix 证明”
```

后续若控制系统、FPGA/LabVIEW 程序、启用端口数或触发协议发生变化，应重新记录现场 pattern，而不是硬编码 7/4007。

## 5. 构建 / 交付状态

Session D 已记录：

```text
full CTest                 = PASS (45/45)
Windows MinGW Debug build  = PASS
Ring/CUDA software selftest= PASS
delivery candidate         = READY
```

该候选二进制对应 source commit `d9daa2d7af6bb8341349e405433824e89e42bcd4`，上表数字是该时点的记录。

**2026-09-25 更正**：此前「canonical main 完成文档/历史集成后 production/test source tree 应保持与 `d9daa2d7` 一致」的说法**已不成立**。2026-09-24 前端链以 fast-forward 进入 main 后，production/test source tree 相对 `d9daa2d7` 已有实际源码改动（41 个文件，+4549 / −618，见第 2 节列出的六个前端链追溯点）。因此：

- A/B/C/D 的语义与验证结论仍由 `d9daa2d7` / `69a7606` 追溯点承载，不因源码树前进而失效，也不需要重新设计；
- 但**不得**再以 `d9daa2d7` 的源码树或二进制身份代表当前 main；当前 main 的源码树身份是 `491aa34`；
- 在 canonical main 上构建时，必须在该 exact SHA 上重新 configure/build 并生成新的 BuildIdentity/交付回执，不得复用旧候选的 binary/BuildIdentity（见 `BUILD_STANDARD.md` §2.3）。

## 6. 远端分支角色

| 角色 | 状态 |
|---|---|
| `main` | 唯一 canonical source/docs baseline。源码树身份 `491aa34`（2026-09-24 前端链 fast-forward）；其后仅文档提交推进 HEAD，接手以最新 `origin/main` 为准 |
| `codex/task-docs` | 任务规格专用分支（48 条独有提交，保留；非生产实现 baseline） |
| `codex/ring-zero-phase-pa-inversion-20260919-025754` | **参考专用 / 不再维护**：阶段 A 算法基准 + 阶段 B1，tip `3032550`、B1 构建源 `d8dd3da` |
| `codex/das-dual-wavelength-quality-b-tier-20260925` | **实验性 / 不再维护、不再实现**：DAS 双波长质量增强 B 档（S2–S4 + D1–D7），tip `2573572`，见 6.3 |
| Session A/B/C/D 分支 | historical / traceability，保留 |
| `codex/physical-round-normalizer-integrated-20260916` | historical validation point，保留 |
| `codex/start-admission-fence-fix-20260913-003112` | historical validation point，保留 |
| 其他旧 `codex/*` / backup / experiment | historical，非默认 baseline |

非破坏性整理原则：不因本次收尾删除远端旧分支，不重写其 history，不 force push。

### 6.1 `codex/ring-zero-phase-pa-inversion-20260919-025754` 的定位（2026-09-24 决定）

```text
分支用途   = 只作参考，不在其上继续实现或维护
tip        = 3032550
B1 构建源  = d8dd3daf2f1bce3afdc01e0452e7ae487f3a1697
```

- 阶段 A 的算法材料（`CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/`）仍是光声反演公式、
  时间轴、滤波顺序/端点与配置拒绝规则的**数值参考**，后续工作不得重新推导这些已收口结论。
- 阶段 B1 任务条款「B1 通过后的 exact SHA 将作为 B2 起点」**作废**。
- 该分支与 canonical `main` 同出于 `fb10721e` 且**至今未合流**；其 `RingRecon/zero_phase_filter.cpp`
  等阶段 B1 产物**不在** canonical `main` 中。若后续任务需要该滤波行为，须在 `main` 上重新
  落地或显式迁移，不得默认认为 `main` 已具备。

### 6.2 2026-09-24 远端分支盘点（本次未删除任何分支）

共 33 条远端分支：

| 分类 | 数量 | 说明 |
|---|---|---|
| 零独有提交、已完整并入 `main` | 18 | 前端链工作分支 5 条 + Session A/B/C/D、`physical-round-normalizer*`、`start-admission-fence-fix`、`integrate-abcd-to-main`、`diagnostics-*`、`docs/readme-main-architecture`、`safety/pre-shm-observability`、`backup/main-pre-diagnostics` 等 13 条历史验证点 |
| 含独有提交、必须保留 | 15 | `codex/task-docs`(48)、`codex/ring-zero-phase-pa-inversion-20260919-025754`(10)、`codex/ring-pipeline-refactor-20260912`(6)、`codex/start-race-validation-20260912-205615`(4)、`codex/host-ingress-protection-experiment-20260908`(4)、`codex/physical-round-blockers-fix-20260916`(3)、`codex/network-ingress-observability-20260907`(3)、`codex/local-history-supplement-20260913`(2)、`backup/local-workspace-source-20260913`(2)、`codex/baseline-role-ack-20260907-132500`(2)、`master`(1)、`backup/sysdelay-per-channel-20260817`(1)、`backup/cf-dmas-pcf-20260816`(1)、`codex/ssh-unattended-test`(1)、`codex/diagnostic-log-export-20260907`(1) |

零独有提交集合的删除不会丢失任何提交（全部仍可由 `main` 抵达），但按本次决定**保留**，
留待单独的清理决策。

### 6.3 `codex/das-dual-wavelength-quality-b-tier-20260925` 的定位（2026-09-25 决定）

```text
分支用途   = 实验性分支，后续不再维护，不在其上继续实现
tip        = 2573572e83538afb1c51318654f89690ef96f6f4
分叉点     = 65de782（canonical main）
独有提交   = 11 条，线性（无 merge commit、无 force push）
回滚锚点   = anchor/00 … anchor/07（本地与远端齐备）
```

**分支性质**：DAS 双波长实时成像质量增强的 B 档实验链（S2 判别性守卫 → S3 滤波分工守卫 →
S4 反演成对开关骨架与 CUDA 侧改造 → 合成体模前向模型 → D1–D7 分析与 D7 环外重跑）。
按本决定停止维护与后续实现。

**可作数值参考的收口结论**（后续工作不得重新推导，但引用须带上下文）：

- 内窥几何（声源全在探测环**外**）下，现有 DAS 权重 `w = Δθ·cosα/d` 的符号约定与几何相反：
  同侧最近探测器 `cosα = −1` 取最大**负**权重，对侧穿行射线 `cosα = +1` 取最大**正**权重；
  图像负能量占比约 50%。
- 环内伪影与真目标同量级（0.92–1.00 × 目标峰值）；**显示层**零值掩膜只改善视觉，不改善
  CR/gCNR/CNR（目标与背景区本就在 `ρ > R`）。
- 建模衰减后，四种权重变体的位置误差收敛；稳健差异仅在「负能量占比」与「环内伪影强度」两项。

**上述结论的适用边界**（不得越过）：合成前向模型无噪声、无折射/透射、无有限探头尺寸；
权重符号问题**未在实测数据上验证**；双极性图像的 CR 因符号相消而偏高，不可直接与单极性图像比较。

**不在 canonical `main` 中的产物**——后续任务不得默认 `main` 已具备：

| 类别 | 文件 |
|---|---|
| 生产源 | `RingRecon/ring_recon.cpp`、`ring_recon.h`、`ring_recon_cuda.cu`、`ring_recon_cuda.h` |
| 新增源 | `RingRecon/RingReconInversion.h`、`RingRecon/RingPhantomForward.h/.cpp` |
| 测试 | `tests/ring_recon_guard_test.cpp`、`tests/ring_phantom_forward_test.cpp`、`tests/CMakeLists.txt`、`tests/frontend_preprocessor_test.cpp` |

canonical `main` 的生产代码保持 B 档前基线：B1 反演开关默认 `Das`、未接线，
全关路径与 prebuilt DLL **逐字节一致**（证据：`CODEX_REPORTS/s4-cuda-switch-20260925/`）。
若后续任务需要其中任何产物，须在 `main` 上重新落地或显式迁移并重新走 BUILD_STANDARD，
不得直接从该实验性分支合入。

## 7. 下一步

1. 以最新 `origin/main` 为唯一 canonical source 基准整理本地工作区。
2. 本地 `main` 只能 fast-forward 到 `origin/main`；若本地存在独有未推送工作，先隔离并报告。
3. 保留 ignored CUDA/runtime、testdata、artifacts、硬件 captures 和历史 build evidence。
4. 在 canonical main HEAD 上重新 configure/build，记录新的 BuildIdentity 和关键依赖 SHA。
5. 后续新功能/修复从 latest `origin/main` 建新任务分支；A/B/C/D 分支不再作为默认开发起点。

## 8. 文档权威层级

1. `PROJECT_STATUS.md`
2. `REPOSITORY_BASELINE.md`
3. `BUILD_STANDARD.md`
4. `Codex-GitHub双端联动快速上手.md`
5. 当前 `codex/task-docs:TASKS/<task>.md`
6. README / 子工程 README
7. `HANDOFF.md` 兼容入口

历史 handoff、receipt、旧任务报告和 validation branches 仅用于追溯，不覆盖当前 main 治理文档。
