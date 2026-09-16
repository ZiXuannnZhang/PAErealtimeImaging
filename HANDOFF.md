# HANDOFF — 当前项目兼容入口

> 本文件仍只作为兼容入口，不替代 `PROJECT_STATUS.md`。
> 新会话、构建、验证、分支整理均应先读取当前 `main` 的项目级文档。

## 当前必须读取的项目级入口

按以下顺序读取：

1. `PROJECT_STATUS.md` — 当前项目状态、活跃/待验证分支、下一步工作。
2. `REPOSITORY_BASELINE.md` — canonical branch、分支角色、历史和 merge 治理。
3. `BUILD_STANDARD.md` — 默认工具链、CMake preset、CUDA/Qt/ZeroMQ、构建与交付规范。
4. `Codex-GitHub双端联动快速上手.md` — ChatGPT / Codex Desktop 协作流程。
5. 当前 `codex/task-docs:TASKS/<task>.md` — 本次任务的精确目标、baseline、commit、测试和显式 override。

## 当前关键状态（2026-09-16）

### Physical round / RoundIdentity

当前主要硬件验收候选：

```text
codex/physical-round-normalizer-integrated-20260916
52cf7713d7e0e935cb14663ec3470f3a25bfeb90
```

软件层状态：

```text
RoundIdentity code fixes = IMPLEMENTED
Automated tests          = PASS
Windows build            = PASS
CUDA service selftest    = PASS
```

但整体状态仍是：

```text
PHYSICAL_ROUND_NORMALIZATION_HARDWARE_ACCEPTANCE = PENDING
```

新控制环境中实机观察到完整轮可能从旧环境约 `4001` 个 physical trigger 变为约 `4007`。当前必须先通过本地日志证明：

- 排除有证据的手动暂停短轮后，完整轮是否稳定为 4007；
- CountBoundary 是否与真实物理圈末一致；
- TimeoutBoundary 是否完整驱动 Ring/UI/ImagingSvc reset、stale drop 和下一轮 clean start。

在这些证据完成前，不能把物理轮次归一标记为已完成，也不能机械把 logical trigger count 从 4000 改为 4006。

### START admission

独立启动 ingress-loss 候选仍为：

```text
codex/start-admission-fence-fix-20260913-003112
6313540f72544c0f68820c4815903abaa0b8c1e1
```

软件侧已 APPROVE，但真实 FPGA/NIC 硬件验证仍 PENDING。该工作流与 4007 / PhysicalRoundNormalizer 实机行为必须分开判断。

### main / task-docs

- `main`：唯一 canonical source / repository-level docs baseline。
- `codex/task-docs`：任务规格通道，不是生产实现基线。
- 两个当前硬件验证候选都保持未合并；软件 APPROVE 不等于硬件 acceptance。

## 当前下一步

当前第一优先级：

1. 本地解析最新大体量诊断日志；
2. 统计完整 physical round 的 distinct trigger count；
3. 验证 CountBoundary 时间线；
4. 验证 TimeoutBoundary 完整 reset 链；
5. 日志没有证据时明确标记“无法判断”；
6. 事实明确前不修改 logical trigger count、first-visible filtering、CountBoundary 语义或 FPGA/UDP；
7. 若实机证据证明现有归一模型不适配新控制环境，再创建独立源码整改任务。

完整约束和状态以 `PROJECT_STATUS.md` 为准。

## 历史迁移记录

2026-09-05/06 的 `realtime_imaging_migration` 原始 handoff 已降级到：

```text
docs/history/HANDOFF_realtime_imaging_migration_20260905-06.md
```

历史记录只用于技术考古，其中旧绝对路径、旧 remote、旧 build cache、本地未提交状态和旧候选状态不得覆盖当前 `main` 项目级文档。
