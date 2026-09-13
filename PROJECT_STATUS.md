# PAERealtimeImaging 当前项目状态

> 更新时间：2026-09-13（UTC+8）
>
> 本文件是**当前项目状态的单一事实入口**。它描述“现在什么是正式基线、哪些分支仍在验证、下一步做什么”。
> 分支/历史治理以 `REPOSITORY_BASELINE.md` 为准；构建与交付以 `BUILD_STANDARD.md` 为准；具体任务的特殊要求以 `codex/task-docs:TASKS/<task>.md` 为准。

## 1. 当前结论

- `main` 仍是唯一 canonical branch，保存正式源码与正式仓库级文档。
- `codex/task-docs` 仍是任务规格专用分支，不是实现基线。
- START admission 软件修复已经完成独立源码审查与确定性软件验收，结论为 **APPROVE**，但**真实 FPGA/NIC 实机测试尚未完成**。
- 因此 START admission 修复**暂不合并 `main`**；当前实机测试候选保持在独立实现分支。
- 用户已手动删除临时分支 `codex/local-docs-sync-20260913`。该分支不是正式成果，不应恢复、重建或作为任何本地整理依据。

## 2. 当前关键分支

| 角色 | 分支 | 当前用途 |
|---|---|---|
| 正式基线 | `main` | 唯一 canonical source / docs baseline；新任务默认从最新 `origin/main` 创建 |
| 任务通道 | `codex/task-docs` | 只保存 `TASKS/` 任务规格与任务通道治理 |
| START admission 实机候选 | `codex/start-admission-fence-fix-20260913-003112` | 软件验收 APPROVE；保持未合并，等待真实 FPGA/NIC 实机测试 |
| Ring pipeline 独立方向 | `codex/ring-pipeline-refactor-20260912` | 独立未合并方向；不得隐式作为当前主线或与 START 候选混合 |
| backup / historical / experiment | 其他 `backup/*`、旧 `codex/*`、`master` 等 | 仅用于追溯；不得作为新任务默认基线 |

START admission 实机候选的已验收远端 HEAD：

```text
codex/start-admission-fence-fix-20260913-003112
6313540f72544c0f68820c4815903abaa0b8c1e1
```

除非后续明确下发追加整改任务，否则实机测试必须针对这个精确 commit 生成构建产物，不得在本地静默加入额外源码修改。

## 3. START admission 当前验证边界

已完成的软件侧结论：

- 修复了 START 串行发送窗口内的 admission race：较早卡在自己的 START 发送后到达的数据不再因为全局 `SourceCore` 尚未 enable 而直接作为普通 `Disabled` 丢弃。
- 引入按卡 START admission state 和 bounded hold/release；成功路径按原始 ingress 顺序释放，失败/overflow fail-closed。
- 修复 `completeStart(false)` 无 callback 场景，保证失败不可被兼容 fallback 反转为成功。
- START analyzer 采用 decision-first 语义，不把 stage 6 的 post-send result timestamp 错当成 admission 下界。
- 确定性 loopback/race、failure、overflow、no-callback、analyzer 和 core/network 回归已经通过并完成独立审查。

尚未完成、因此不能声称已解决的部分：

- 真实 FPGA 收到 START 后首包/首触发时序；
- 真实 NIC / driver / Windows 网络栈在启动 burst 下是否存在独立丢包；
- 现场触发跳号是否只由已修复的软件 admission race 导致；
- 真实系统抓取与应用 stage 1/stage 2 在问题轮次的完整因果闭环；
- 高负载实机下下游 sync queue、保存和成像链路是否出现独立淘汰。

因此当前结论必须表述为：

```text
START admission 软件竞态已修复并通过软件验收；真实 FPGA/NIC 现场问题仍待实机验证。
```

不能表述为：

```text
实机丢包问题已经解决。
```

## 4. 实机测试构建规则

虽然测试对象不在 `main`，构建规范仍读取当前 `origin/main:BUILD_STANDARD.md`。

实机测试 START admission 候选时：

```text
Build target branch = codex/start-admission-fence-fix-20260913-003112
Build target SHA    = 6313540f72544c0f68820c4815903abaa0b8c1e1
Build policy        = latest origin/main:BUILD_STANDARD.md
```

`main` 上新增或更新的治理/构建文档不意味着应该把 `main` 的源码混入候选分支。构建代理应在候选 commit 上构建，同时按 `BUILD_STANDARD.md` 生成 BuildIdentity、完整 runtime、manifest 和 SHA-256 回执。

## 5. 当前生产/主线架构

`main` 的正式采集架构仍是 PAimage-derived backend：

```text
FPGA cards
  -> Windows UDP
  -> single PaimageAcquisition::SocketReceiver
  -> SourceCore
  -> HostOutput / FrameConverter
  -> DataProcessor output interfaces
  -> display / ring realtime / save / publisher
```

旧 `MultiPortReceiver` 仍可存在于兼容和测试路径，但不是当前生产 UDP 入口。

当前采样模型：

```text
sample interval = 4 ns = 250 MSa/s
A + B           = 32 bit/channel = 8 bytes/sample pair
UDP payload     = 1440 bytes (+ 4-byte protocol header)
samples/trigger = acqTimeNs / 4
```

200 us / 200 Hz 时 payload 约为：单卡 80 MB/s，4 卡 320 MB/s，8 卡 640 MB/s，16 卡 1.28 GB/s；未包含 Ethernet/IP/UDP、IFG、driver、诊断和用户态处理开销。

## 6. 下一步工作

当前优先级：

1. 依据 `BUILD_STANDARD.md` 为 `6313540f...` 生成可追溯实机测试包。
2. 在真实 FPGA/NIC 环境执行多轮 START/STOP 与目标负载测试。
3. 同步采集应用 stage 1/stage 2/control trace，必要时使用 Pktmon/WPR 系统抓取。
4. 区分：START admission、NIC/driver/socket、SourceCore first-arrival assembly、下游 sync queue 等不同层级问题。
5. 实机验收通过后，再决定是否把 START admission 分支以保留历史的方式集成到 `main`。

在第 5 步之前，不得仅因软件测试 APPROVE 而提前合并。

## 7. 文档权威层级

当前操作应按以下入口理解：

1. `PROJECT_STATUS.md` — 当前项目/分支/验证状态。
2. `REPOSITORY_BASELINE.md` — canonical branch、分支角色、历史与 merge 治理。
3. `BUILD_STANDARD.md` — 默认工具链、CMake preset、CUDA/Qt/ZeroMQ、产物与交付规范。
4. `Codex-GitHub双端联动快速上手.md` — ChatGPT/Codex 双端工作流。
5. 当前 `TASKS/<task>.md` — 具体任务要求和显式 override。
6. `README.md` 与各子工程 README — 架构与使用说明。

历史 Handoff、迁移记录、旧任务报告、旧绝对路径和旧 build cache 说明只用于追溯，**不得覆盖上述当前文档**。

## 8. 历史文档降级

原根目录 `HANDOFF.md` 的 2026-09-05/06 `realtime_imaging_migration` 内容已经不再代表当前仓库状态，其中包含旧工作区、旧 remote、`build/mingw_make`、未提交改动和旧迁移阶段操作说明。

原文已作为历史资料保留在：

```text
docs/history/HANDOFF_realtime_imaging_migration_20260905-06.md
```

根 `HANDOFF.md` 只保留历史兼容入口和当前权威文档指针，不再作为“新会话唯一入口”。

## 9. 本地工作区整理原则

后续允许 Codex 以远端仓库为标准整理本地工作区时，应遵守：

- 先 `git fetch --prune origin`，让已被用户删除的远端分支同步消失；
- 本地 `main` 只能 `--ff-only` 跟随 `origin/main`；发生分叉必须停止报告；
- 不得为“对齐远端”自动 `reset --hard`、rebase、force push；
- 不得删除被 `.gitignore` 排除但构建所需的本地 CUDA/runtime 依赖；
- 不得删除 `artifacts/`、build evidence 或用户数据，除非任务明确授权；
- 当前 START admission 候选分支应保留并核对远端 HEAD `6313540f...`；
- 历史本地分支可在确认远端已有可追溯副本且无独有未推送提交后再清理。
