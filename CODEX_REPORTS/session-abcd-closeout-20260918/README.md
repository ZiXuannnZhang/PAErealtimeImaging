# Session A–D closeout archive — 2026-09-17/18

> **历史归档。** 当前项目状态以仓库根目录 `PROJECT_STATUS.md` 为准。

## 1. 归档目的

本目录集中保存 PhysicalRound / RoundIdentity 本轮 Session A–D 的执行与验收证据，避免阶段 handoff/receipt 长期占据仓库根目录。

最终 accepted source lineage：

```text
START admission software point
  6313540f72544c0f68820c4815903abaa0b8c1e1

PhysicalRound retained integrated point
  52cf7713d7e0e935cb14663ec3470f3a25bfeb90

Session A final
  eb283f64574d9043f4d4823338c31767c3b811fe

Session B final
  676df4a946fed56b903474354984bff44ab50875

Session C accepted production/test source
  d9daa2d7af6bb8341349e405433824e89e42bcd4

Session D final traceability HEAD
  69a7606f95c97c839fd618115f4092a4291d8906

canonical integration main
  36bb556441dedd760ab363f3df4f38872bbfc93f
```

## 2. Session summary

### Session A — core round policy

- configurable `startupFilterTriggerCount`；
- configurable `disableCountBoundary`；
- shared multicard distinct-physical classification / RoundIdentity；
- current/last-completed physical/filter counters；
- software review APPROVE。

### Session B — persistence / diagnostics / production gap accounting

- `RingConfigDialog/Defaults` 单一持久化路径；
- UI status semantics 冻结；
- production full-trigger gap owner 移至 SourceCore；
- same-session triggerSeq large-backjump reset recovery，阈值 256；
- software review APPROVE。

### Session C — variable-length physical round

- disable=false 保留 CountBoundary；
- disable=true：configured count 仅为 realtime imaging cap，raw/save 继续；
- 20 ms poll chain 成为 active idle timeout owner；
- timeout AutoSave binding、capture-before-reset、Ring/CUDA reset/stale cutoff；
- no synthetic completion；
- software review APPROVE。

### Session D — final integration validation

- 无 production/test code bugfix；
- policy matrix / variable-round scenarios / focal regressions PASS；
- full CTest 45/45 PASS；
- Windows full build PASS；
- real ImagingSvc + CUDA selftest PASS；
- formal delivery candidate READY。

用户随后实机测试反馈：**当前验证范围内暂未发现与预期不符合的行为**。当前正式定级见根目录 `PROJECT_STATUS.md`：

```text
A/B/C/D functional validation in current tested scope = PASS
exact FPGA/LabVIEW source of extra startup triggers   = NOT PROVEN
7 / 4007                                              = field observation, not protocol constant
```

## 3. 保留文件

- `HANDOFF_SESSION_D_20260918.md` — 最终 A/B/C 汇总 handoff；
- `HARDWARE_VALIDATION_CHECKLIST_SESSION_D_20260918.md` — 实机前的历史 checklist，文件内 PENDING 状态代表当时阶段，不是当前状态；
- `receipts/` — A/B/C/D 精确执行、addendum、构建与测试回执。

A/B/C 单独 handoff 已被本文件 + Session D handoff + receipts 覆盖，因此不再保留在当前树；如需原文可从 canonical integration 前 Git history 恢复。
