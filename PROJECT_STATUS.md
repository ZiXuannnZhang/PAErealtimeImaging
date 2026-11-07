# PAERealtimeImaging 当前项目状态

> 更新时间：2026-11-07（UTC+8）。分支治理见 [REPOSITORY_BASELINE.md](REPOSITORY_BASELINE.md)，构建交付见 [BUILD_STANDARD.md](BUILD_STANDARD.md)。

## 1. 当前结论

用户于 2026-11-07 确认此前未验收的环形实时成像工作通过实机验收。
已验收源码及测试包 BuildIdentity 为 `4c83ecf7b0dc0e0be8ca901ca952b7521f79393a`。用户另行确认将
`codex/ring-realtime-performance-20261104` 普通快进到远端 `main`；后续开发从最新 `origin/main` 开始。

本轮包括同心圆折射精确核 FBP、分段和融合 GPU 预处理、按轮次和触发序号有界多卡配对、
顺逆九点钟起步，以及方向联动按物理轮生效、纯换向不重启成像服务。退役旧 FilteredDAS 和可选
DAS 权重控件；时变低通仍可用。实机验收按用户测试范围记录，八通道性能专项仍暂缓。
详见 [验收记录](CODEX_REPORTS/ring-realtime-acceptance-20261107/README.md)。

- 本轮以普通快进将实现分支接入 canonical `main`；接入前的远端 `main` 是 `6ae7e8940ca7088bb4ea01415da9e303f301a24a`，为验收源码的祖先。2026-11-04 已验收包 `1fb9478` 及 [验收记录](CODEX_REPORTS/main-acceptance-20261104/README.md) 保留历史追溯。
- 本轮验收发布分支：`codex/ring-realtime-performance-20261104`。验收子树 `MC_410T_MultiCard` 为 `8da942be1a382bfff8e6b4520424a58b0a461a7c`；文档收尾保持该子树不变。
- A/B/C/D 及发现早退 A / count-boundary 的实机结论沿用原已测范围。Layer 2b T_arr 未独立测量，发现早退 B 待决定，额外 trigger 的 FPGA/LabVIEW 来源未证明。

## 2. 当前 main 的环形成像实现

- 在保留 START-admission、PhysicalRoundNormalizer、RoundIdentity、超时/保存隔离及前端链的基础上，新增同心圆折射精确核 FBP。
- 保留分段和融合 FBP 两条路径，复用同一精确核和反投影；FBP 优先使用融合 GPU 预处理，分段路径保留。GPU STFT 使用批量 cuFFT；DAS 保留可选 STFT，旧 FilteredDAS 和可选权重控件退出生产。
- 多物理卡按物理轮次与逻辑触发序号有界配对，按物理通道升序组块。瞬时队列锁竞争不再产生 `QueueBusy` 丢弃；真正的缺口由 `reorderIncompleteBlocks` 记录，直存链路不变。
- 顺、逆均从九点钟起步，启用通道沿电机方向等分扇区。电机文件只联动 `direction`，其他字段不改成像配置；纯换向在下一物理轮生效，不重启成像服务，前端反馈等待下一轮和首帧。首次联动若改变手动起点，仍按起点变化重新配置。

## 3. 验收、构建身份与追溯

| 对象 | 身份 / 状态 |
| --- | --- |
| 已验收源码 / 包内 BuildIdentity | `4c83ecf7b0dc0e0be8ca901ca952b7521f79393a` |
| 本地验收 ZIP | `artifacts/build-delivery/20261107-215839_4c83ecf7_ring_queue_direction_local.zip` |
| 软件回归 | 57/57 CTest、1/1 MainWindow；包内 2-A、4-A 顺逆 × 分段/融合 FBP 四组自检 |
| 实机结论 | 用户确认此前未验收工作全部通过；限实际测试范围 |
| 追溯 | [本轮验收与包校验](CODEX_REPORTS/ring-realtime-acceptance-20261107/README.md) |

本次文档收尾不重标旧包，不重建二进制。无新增现场日志或逐圈计数，八通道性能专项仍暂缓；
既有 7 / 4007 继续作为现场观察而非协议常量。

## 4. 冻结语义

- RoundIdentity=(measurementSession, roundGeneration) 是 Normalizer/Ring/ImagingSvc/UI/save 的 ownership key。
- startupFilterTriggerCount=X 过滤前 X 个 new distinct physical trigger identities，多卡共享分类。
- disableCountBoundary=false：configured logical count 保持 CountBoundary/final 语义。
- disableCountBoundary=true：configured count 是成像 cap，raw/save 和前端信号按已接入规则继续到 timeout。
- active physical-idle timeout 由 20ms PAimage poll 链单一负责，partial-startup round 也可 timeout。
- TimeoutBoundary 顺序：同步 AutoSave binding → 捕获旧轮 presentation → Ring/CUDA reset/stale barrier；
  PNG disk write 可异步。旧 raw write 保持旧 generation。
- 不生成 synthetic partial Ring block，不修改 expectedBlocks，不强制 variable/early round completion。
- 缺失=triggersPartial，跳号数=missingTriggerCount，丢包=packetsDropped。
- full-trigger gap owner：SourceCore → Decision::TriggerGap → CardStats。
- same-session triggerSeq 大幅回退恢复阈值 256；small backstep 不 re-anchor。
- raw/save、frontend clone 和成像队列隔离；成像拒绝不算 UDP ingress loss，不改保存 ownership。
- 7 / 4007 仍为现场观察与配置，不是协议常量；额外 trigger 精确 FPGA/LabVIEW 来源未证明。

## 5. 分支角色与下一步

新任务默认从 latest `origin/main` 新建任务分支。本轮实现发布分支为 `codex/ring-realtime-performance-20261104`；
已包含原低通分支 tip `3340fbf` 和精确 FBP 阶段 tip `21d60c0`，历史 refs 保留。
发现早退 B（本地绑定默认值和 tooltip）仍待决定；`codex/task-docs` 不是生产实现。
ring-zero-phase 仅作数值参考，DAS B-tier 继续停维护。Layer 2b T_arr、额外 trigger 根因
及全孔径八通道正式校准保留原验证边界。

## 6. 本地工作区与保留

本地旧 `main`（`d3054d51e19570a6ef38774b8a82326738dce53b`）有独有文档/开发配置提交，
与 canonical 链分叉，原样保留，不 reset/rebase、不隐式接入。
未跟踪文件和 frozen handoff 保持原样；原始数据、校准和硬件证据不随本轮源码推送。
旧 `_pending_delete` 的最终删除仅由用户执行。
