# PAERealtimeImaging 当前项目状态

> 更新时间：2026-10-13（UTC+8）。当前 main 是唯一源码、文档与下一步开发基准。
> 分支治理见 REPOSITORY_BASELINE.md，构建交付见 BUILD_STANDARD.md；具体任务要求优先。

## 1. 当前结论

用户已决定将环形增强的最新内容接入 main，并确认采用线性接入方案：
在旧 main **6ff0e02fac0d6fe0e5a4edd089c09c85d18ff5fa** 上建立普通单父提交，
生产与测试源码完整取自 **bd697851d2a1b6f11c39c06baf7308f1cac372ce**，
保留并更新 main 的一级项目文档。随后普通快进推送并同步本地 main。
原分支及历史继续保留；没有 merge commit、force push、rebase 或既有历史改写。

旧 main 与环形分支原本分叉（共同祖先 c95917f，双方独有 4 / 17 个提交），
无法直接把 main 指向 bd69785。此次是内容线性接入：
**bd69785 的源码已进入 main，但 bd69785 本身不因此成为 main 的祖先。**

    CANONICAL_BRANCH                     = main
    CANONICAL_MAIN_HEAD                  = latest origin/main
    IMPORTED_SOURCE_REFERENCE            = bd697851d2a1b6f11c39c06baf7308f1cac372ce
    MC_410T_MULTICARD_TREE                = ff2e987a3f44565496ff57f8467e891ea04757d0
    NEXT_DEVELOPMENT_BASELINE             = latest origin/main
    HISTORY_POLICY                       = preserve original branches and commits
    A_B_C_D_HARDWARE_VALIDATION           = PASS_FOR_CURRENT_SCOPE
    DISCOVERY_A_HARDWARE_VALIDATION       = PASS_FOR_CURRENT_SCOPE (2026-10-01)
    COUNT_BOUNDARY_HARDWARE_VALIDATION    = PASS_FOR_CURRENT_SCOPE (2026-10-01)
    LAYER_2B_T_ARR_MEASUREMENT            = NOT_EXECUTED
    BIPOLAR_COMPENSATION_EFFECT_TUNING    = SHELVED_BY_USER
    DISCOVERY_B_DISPOSITION              = PENDING_USER_DECISION
    FPGA_LABVIEW_EXTRA_TRIGGER_SOURCE    = NOT_PROVEN

## 2. 进入当前 main 的内容

- 原 main 已接受的 START-admission、PhysicalRoundNormalizer、RoundIdentity、timeout、
  realtime imaging cap、AutoSave binding、Ring/CUDA reset/stale barrier。
- 2026-09-24 前端链：全分辨率显示裁切、异步 Frontend Preprocessing Stage、逐 A-line
  零相位滤波、保存/闸门与写盘故障修复、卡片状态与通道显示命名。
- 环形增强：RingSignalEnhancer 的双极波形补偿与低频补偿 H(f)，FFT A1/A2/A3 优化，
  显示层零值圆形掩膜与毫米坐标。A4 未实施、A5 未派发，无 RingReconCudaConfig ABI 变化。
- 发现早退 A 范围：达期望卡数后提前结束发现，移植提交 6744787。
- count-boundary 前端解耦：勾选“禁用计数重置”并超过设计触发数后，时域/频域持续刷新，
  publisher 和 Ring 成像保持冻结，保存与卡片计数照常；未勾选模式保持原语义。
- 上述分支内 CODEX_REPORTS 证据与交付回执随内容接入，保留原始记录口径。

将内容接入 main 不扩大既有硬件验收范围。双极补偿实现仍在源码中，但效果调优已搁置；
实时性正式结论仍依赖尚未执行的 Layer 2b T_arr 实测，当前结论具有条件性。

## 3. 关键追溯点与交付包

| 对象 | 提交 / 身份 |
|---|---|
| START-admission 接受点 | 6313540f72544c0f68820c4815903abaa0b8c1e1 |
| PhysicalRound 集成点 | 52cf7713d7e0e935cb14663ec3470f3a25bfeb90 |
| Session A / B | eb283f6 / 676df4a |
| Session C 接受源码 | d9daa2d7af6bb8341349e405433824e89e42bcd4 |
| Session D 最终追溯点 | 69a7606f95c97c839fd618115f4092a4291d8906 |
| 旧 main 前端链源码身份 | 491aa34cfa9553954eea949a7703df7194eb7699 |
| count-boundary 功能提交 | aa9097ef126ab3ea4a2a1ce2c5786ecaba7cd184 |
| 已有轻量包构建提交 | 3c6323dcdf587322c896f8039868eca19ba5a3d5 |
| 本次环形内容输入 tip | bd697851d2a1b6f11c39c06baf7308f1cac372ce |

PAimage_CountBoundaryFrontendOnly_aa9097e_lightweight 实际构建于 3c6323d，tracked dirty=false。
其生产/测试源码与 aa9097e、bd69785 完全一致；bd69785 仅追加交付回执。
2026-10-13 只读核查确认主程序内嵌 SHA、运行记录及哈希一致，清单内 22 个 EXE/DLL
匹配，只有 INI 与交付清单不同。本次接入不重新构建、不产生新的二进制身份。
后续正式交付必须在新的 exact main SHA 重新 configure/build 并记录 BuildIdentity。

历史 Session D 的 CTest 45/45、环形/count-boundary 的 CTest 51/51 均为原提交的证据，
不宣称为新 main SHA 上重新执行的测试。count-boundary 的部分 .log 未归档 .txt 的证据
缺口继续保留，不能因此将已记录的实机通过改写为未通过。

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

main 是下一步开发基准。环形增强和 count-boundary 的内容已接入 main，原 ref 保留追溯；
不能把“源码相同”误写为“原 tip 已是 main 祖先”。discovery A 范围已进入 main，B 范围
（本地绑定默认值与 tooltip 文案）仍留原分支待用户决定。task-docs 只保存任务规格；
最新 postFilter harness 派发不代表新实现已完成。

ring-zero-phase-pa-inversion 仍只作数值参考、不再维护；其 B1→B2 起点约定作废。
DAS B-tier 实验分支继续不维护，其源码、测试、文档与结论不作为 canonical 事实。
新功能默认从 latest origin/main 新建任务分支。完整历史盘点可查旧 main 6ff0e02 的文档。

## 6. 本地清理

用户于 2026-10-13 明确授权：先核实源码远端备份，只保留最新 main 的活跃源码和文档；
可再生构建/测试产物移入根目录 _pending_delete/<task>/ 待用户审核后自行删除。
未备份源码先建立单独远端备份，不纳入 main。原始采集、校准结果、硬件证据和未确认可再生
数据单独标记“保留、不可删除”，不混入可删除集合。代理不执行最终删除，也不清空旧暂存区。

权威顺序：PROJECT_STATUS → REPOSITORY_BASELINE → BUILD_STANDARD → HANDOFF →
Codex-GitHub双端联动快速上手 → 子工程 README → CODEX_REPORTS。
任务明确要求可覆盖冲突条款。
