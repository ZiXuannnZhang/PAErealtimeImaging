# HANDOFF — PAERealtimeImaging 当前接力入口

> 更新时间：2026-10-13（UTC+8）。当前基准始终取 latest origin/main。

用户已确认将环形增强 bd69785 的完整源码/测试内容线性接入旧 main 6ff0e02，
建立普通单父提交并普通快进推送。没有 merge commit、force push 或旧历史改写。
旧环形与 count-boundary 分支保留追溯；内容已进入 main，不代表旧 tip 已成为 main 祖先。

## 当前状态

- 下一步开发从 latest origin/main 开始，生产/测试源码与 bd69785 完全一致。
- START-admission、Session A/B/C/D 已收尾，当前验证范围实机通过。
- 发现早退 A 范围、count-boundary 前端解耦于 2026-10-01 实机通过。
- 双极补偿效果调优搁置；Layer 2b T_arr 尚未执行，实时性结论仍为条件性。
- discovery B 范围归宿待用户决定；postFilter 只有任务派发记录，不代表实现完成。
- 7 / 4007 不是协议常量，额外 trigger 的 FPGA/LabVIEW 精确来源仍未证明。
- 当前 main 已含环形信号增强、FFT 优化、掩膜/毫米坐标、发现早退 A 和 count-boundary；
  不再从旧分支的“尚未并入 main”状态推断当前能力。

## 生产链

    FPGA → SocketReceiver → SourceCore → HostOutput
      CardFrame → save queue → FileSaver
      SyncFrame → DataProcessor::deliverAssembled
        → FramePublisher (raw group, optional)
        → FrontendPreprocessor (async clone / filter / display preparation)
          → DisplayBuffer
          → Ring feed → RingBlockAssembler → ImagingController → ImagingSvc → ring_recon_cuda

raw/save 与 frontend clone 隔离。超 cap 的 frontend-only 标记使 publisher 与 Ring 冻结，
前端信号继续刷新；旧 MultiPortReceiver/PacketAssemblyBuffer 不是生产 UDP ingress owner。
CONFIG 成功需要当前事务全卡 60-byte ACK，18-byte ready packet 不替代 ACK。

## 接手与交付

先读 PROJECT_STATUS.md、REPOSITORY_BASELINE.md、BUILD_STANDARD.md，再读本文件与协作指南；
具体任务要求以 codex/task-docs:TASKS/<task>.md 为准。历史 receipt 不覆盖当前状态。

正式构建按 BUILD_STANDARD.md，在待交付 exact SHA 重新 configure/build、记录 BuildIdentity、
tracked-clean、工具链、runtime 来源和 SHA256。旧包构建 3c6323d 与当前源码相同，
但不能把旧 binary identity 改称新的 main SHA。本次接入不生成新二进制或硬件验收结果。

RoundIdentity、startup filter、timeout/save/reset 顺序与统计语义以 PROJECT_STATUS.md §4 为准。
修改 RingReconCudaConfig 时必须同步核对所有 DLL/consumer ABI。

ring-zero-phase 分支只作数值参考；DAS B-tier 分支停止维护，其结论不进入 canonical 状态。
RadiusCalibration 仍需真实全孔径校准数据，不对 raw Bscan 做 circshift；
尚无“8 通道正式全量校准完成”结论。_migration_pack 的 frozen provenance 文件保持原样。

## 本地工作区

当前 main 是唯一活跃源码/文档基准。旧副本、可再生构建和测试结果按本轮授权移入
_pending_delete；源码先核实远端备份，未备份的原始数据与证据单独保护。
只有用户执行最终删除。不要清空旧 _pending_delete，不用 reset --hard / clean 强制同步。

## 2026-10-16 实现候选（尚未合入 main）

`codex/ring-timevar-lowpass-20261016` 从批准基线 a8c15cff 移除旧双极/低频增强，接入仅 Ring 服务消费的 STFT 时变低通。
默认关闭，45/1 MHz、780/2000；执行位置在完整 A-line 的 DBR/限幅/DelayCut 前。
新配置、主/高级草稿、默认保存和启动恢复已接线；独立 MATLAB 数值与软件回执见
`CODEX_REPORTS/ring-timevar-lowpass-20261016/`，模块说明见 `MC_410T_MultiCard/delivery/docs/环形重建增强.md`。
这是实现分支候选，不更改 canonical main 接受状态；硬件/视觉验收与真实 T_arr 仍未验证。
