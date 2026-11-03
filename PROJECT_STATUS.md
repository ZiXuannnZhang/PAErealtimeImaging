# PAERealtimeImaging 当前项目状态

> 更新时间：2026-11-04（UTC+8）。canonical 源码、文档与下一步开发基准为 latest origin/main。
> 分支治理见 REPOSITORY_BASELINE.md，构建交付见 BUILD_STANDARD.md；用户当前明确要求优先。

## 1. 当前结论

截至 2026-11-04，用户确认本地最新交付包及本分支此前工作均已通过实机测试，并授权将
`codex/ring-lowpass-motor-linkage-20261029` 普通 fast-forward 接入 `main`。
已验收源码为 `1fb9478e71c95815671c5e1a2c1b3d6a302746d1`；本次只追加治理文档，不修改已验收源码。

当前能力包括原采集/前端/轮次链、圆形掩膜与毫米坐标、发现早退 A、count-boundary 前端刷新解耦，
以及 Ring STFT 时变低通、FilteredDAS、电机方向/步数参数联动、参数文件选择与持久化、物理轮内冻结及方向映射修正。
旧双极/低频补偿已移除；两项新滤波默认关闭。面板“逆”映射 `sectorCcw=0`、起点 180°，
“顺”映射 `sectorCcw=1`、起点 0°；桥接配置保持 `DirectionInverted=false`。

验收包保留 `1fb9478` 的 BuildIdentity；本次文档提交不产生新二进制。
Layer 2b T_arr 未取得独立实测记录，discovery B 待决定；7 / 4007 仍是现场观察而非协议常量，
额外 trigger 的精确 FPGA/LabVIEW 来源未证明。验收追溯见 `CODEX_REPORTS/main-acceptance-20261104/README.md`。

用户此次确认覆盖该分支已交付工作，包含最新方向修正；取代此前“仅本地、不推送”的限制。
旧远端 main a8c15cf 是已验收源码 1fb9478 的祖先，本次直接快进，无需内容重接或 merge commit。
实机结论来自用户确认；本轮没有新增现场日志或截图，不补写未提供的操作参数、时序数字或根因结论。
历史报告的“待实机/未合 main/仅本地”保留原时间语境，当前接受状态以本文件为准。

    CANONICAL_BRANCH                     = main
    CANONICAL_MAIN_HEAD                  = latest origin/main
    ACCEPTED_SOURCE_SHA                  = 1fb9478e71c95815671c5e1a2c1b3d6a302746d1
    MC_410T_MULTICARD_TREE                = f178261cbf95cfdf937af01ee705f26f7c034f5a
    PREVIOUS_REMOTE_MAIN                 = a8c15cff203da959bd935120605e69dc32ff7145
    INTEGRATION_METHOD                   = normal fast-forward; documentation-only closeout
    LOWPASS_FILTEREDDAS_MOTOR_HARDWARE    = PASS_FOR_USER_TESTED_SCOPE (confirmed 2026-11-04)
    A_B_C_D_HARDWARE_VALIDATION           = PASS_FOR_CURRENT_SCOPE
    DISCOVERY_A_COUNT_BOUNDARY_HARDWARE   = PASS_FOR_CURRENT_SCOPE (2026-10-01)
    LAYER_2B_T_ARR_MEASUREMENT            = NOT_EXECUTED
    LEGACY_BIPOLAR_LOWFREQ_ENHANCEMENT    = REMOVED_FROM_PRODUCTION
    DISCOVERY_B_DISPOSITION              = PENDING_USER_DECISION
    FPGA_LABVIEW_EXTRA_TRIGGER_SOURCE    = NOT_PROVEN

## 2. 当前 main 的内容

- 保留 START-admission、PhysicalRoundNormalizer、RoundIdentity、timeout、成像 cap、AutoSave
  binding、Ring/CUDA reset/stale barrier，以及前端异步预处理、显示裁切、滤波、保存和卡片状态链。
- 保留发现早退 A、count-boundary 前端解耦、圆形显示掩膜与毫米坐标。
- 移除旧双极/低频补偿及其控件、配置和生产算法，使用 Ring STFT 时变低通；它在完整 A-line 的
  DBR/限幅/DelayCut 前执行，默认关闭。旧增强专用 FFT 优化不再列为当前生产能力。
- 新增独立 FilteredDAS 预滤波；两种滤波可独立组合，配置原子接受，失败不提交半块 CUDA/WL2 历史。
- 修复配置被拒绝后 MainWindow 仍启动 Ring 的问题；修复 Normalizer newest-first 缓存遍历。
- 电机参数文件联动方向、起点及配置允许的圈长规则；“参数文件选择”可浏览 JSON，
  路径写入 INI 并在启动恢复。物理轮首触发冻结，CountBoundary/TimeoutBoundary 后允许应用，
  startupFilter=0 同样生效；服务停止不冒充物理轮结束，失败保留待重试，编辑窗口开启时暂缓。
- 实机方向修正：逆/CCW → sectorCcw=0、180°（九点钟）；顺/CW → sectorCcw=1、0°（三点钟）。
  DirectionInverted=false，输入文字和桥接含义不反转，无联动手动路径保持。验收包使用 directionOnly，
  不将本次用户确认写成全部步数规则组合的逐项实机测量。
- raw/save、共享 frontend、CUDA/SHM ABI 和既有轮次 ownership 保持；本次接入不新增源码改动。

## 3. 验收、构建身份与追溯

| 对象 | 身份 / 状态 |
|---|---|
| 原 canonical main | a8c15cff203da959bd935120605e69dc32ff7145 |
| 原低通/FilteredDAS 分支 tip | 3340fbff5ed93ff2ba87b496275e663e877fa693（保留） |
| 电机 bundle 原始实现 / 报告 | 04e348b / 50f41e5，移植为 20c46e9 / a16264a |
| 联合修复 / 文件选择 | 0c5147a / 39b8ec9 |
| 实机已验收源码 / BuildIdentity | 1fb9478e71c95815671c5e1a2c1b3d6a302746d1 |
| 验收包 | artifacts/build-delivery/20261029-213219_1fb9478e_lowpass_motor_local_direction/ |
| 本次确认与核查记录 | CODEX_REPORTS/main-acceptance-20261104/ |

上述 exact 1fb9478、tracked-clean 的标准 configure/build、56/56 CTest、真实 MainWindow
入口 1/1 与合成服务 selftest 均已通过。32 组服务矩阵覆盖 4/8 通道、两个方向、拼接和两滤波开关。
既有 MATLAB 数值、旧 OFF 字节一致性及性能证据分别保留在 ring-timevar-lowpass、ring-filtered-das 报告，
不改写为本轮重新执行的性能实验。用户此次确认覆盖该分支已交付功能的实机测试范围。

本次仅文档收尾，生产/测试子树与 1fb9478 一致；不重新构建、不修改验收包或旧回执。
包内 local_only=true、旧远端 SHA 和当时 UNVERIFIED 文案保持原样，当前验收/发布状态由本次记录补充。
将来若交付新的 main exact SHA，仍须重新 configure/build 并记录新 BuildIdentity。

历史 START 6313540、PhysicalRound 52cf771、A/B eb283f6/676df4a、C d9daa2d7、D 69a7606f、
前端链 491aa34、count-boundary aa9097e、旧轻量包 3c6323d、旧增强内容输入 bd69785 保留追溯。
2026-10-13 曾将 bd69785 内容线性接入 main；原 tip 并非因此成为 main 祖先。本次则为真实祖先链快进。
旧 count-boundary 部分 .log 未归档 .txt 的证据缺口继续保留。

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

新工作默认从 latest origin/main 新建任务分支。联合功能分支作为本次已验收集成追溯保留；
原低通分支 codex/ring-timevar-lowpass-20261016@3340fbf 已被 main 祖先链包含，原 ref 不移动。
发现早退 B（本地绑定默认值和 tooltip）仍待用户决定；task-docs 派发不能当作生产实现。
ring-zero-phase-pa-inversion 仅作数值参考，不维护；B1→B2 起点约定作废。
DAS B-tier 实验继续不维护，其旧结论不因本次独立 FilteredDAS 实现进入 main 而被接受。
Layer 2b T_arr、额外 trigger 根因和全孔径 8 通道正式校准仍各自保留原边界。

## 6. 本地工作区与保留

本地旧 main d3054d51e19570a6ef38774b8a82326738dce53b 含一份独有文档/开发配置提交，
与本次实现分叉，按治理规则原样保留，不 reset/rebase、不隐式接入。远端 main 独立正常快进；
当前联合分支在发布后与 origin/main 同步。其他未跟踪文件和 frozen handoff 均不改动。
旧 _pending_delete 最终删除仅由用户执行；原始数据、校准和硬件证据保持保护，不新增清理授权。
