# PAERealtimeImaging 当前项目状态

> 更新时间：2026-09-18（UTC+8）
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
REMOTE_BRANCH_CLEANUP                     = NON_DESTRUCTIVE
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

该候选二进制对应 source commit `d9daa2d7af6bb8341349e405433824e89e42bcd4`。

canonical main 在完成远端文档/历史集成后，其 Git SHA 会变化，但 production/test source tree 应保持与 `d9daa2d7af6bb8341349e405433824e89e42bcd4` 一致。执行代理随后按远端 main 整理本地工作区时，应在新的 canonical main HEAD 上重新 configure/build 并生成新的 BuildIdentity/交付回执；这属于 canonical-main 构建追溯，不表示需要重新设计 A/B/C/D 语义。

## 6. 远端分支角色

| 角色 | 状态 |
|---|---|
| `main` | 唯一 canonical source/docs baseline |
| `codex/task-docs` | 任务规格专用分支 |
| Session A/B/C/D 分支 | historical / traceability，保留 |
| `codex/physical-round-normalizer-integrated-20260916` | historical validation point，保留 |
| `codex/start-admission-fence-fix-20260913-003112` | historical validation point，保留 |
| 其他旧 `codex/*` / backup / experiment | historical，非默认 baseline |

非破坏性整理原则：不因本次收尾删除远端旧分支，不重写其 history，不 force push。

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
