# HANDOFF — PAERealtimeImaging 当前接力入口

> 更新时间：2026-11-07（UTC+8）。新开发基准仍为 latest `origin/main`。

用户于 2026-11-07 确认此前未验收的环形实时成像工作通过实机验收。
已验收源码及测试包 BuildIdentity 为 `4c83ecf7b0dc0e0be8ca901ca952b7521f79393a`。实现与验收文档通过
`codex/ring-realtime-performance-20261104` 发布；canonical 开发基线仍是 `origin/main`。

本轮包括同心圆折射精确核 FBP、分段和融合 GPU 预处理、按轮次和触发序号有界多卡配对、
顺逆九点钟起步，以及方向联动按物理轮生效、纯换向不重启成像服务。退役旧 FilteredDAS 和可选
DAS 权重控件；时变低通仍可用。实机验收按用户测试范围记录，八通道性能专项仍暂缓。
详见 [验收记录](CODEX_REPORTS/ring-realtime-acceptance-20261107/README.md)。

## 接手要点

先读 [PROJECT_STATUS.md](PROJECT_STATUS.md)、[REPOSITORY_BASELINE.md](REPOSITORY_BASELINE.md)
和 [BUILD_STANDARD.md](BUILD_STANDARD.md)，再读协作指南。查看实现分支时明确与 canonical 主线的差别。

- 成像队列使用有界 FIFO。组包按 RoundIdentity 和逻辑触发序号配对；缺失超过窗口时跳过不完整块，
  保留后续角度及双波长相位。直存队列、raw/save ownership 与成像分离。
- 电机只联动 `direction`；每圈总 A-line 数由成像参数设定。顺逆均从九点钟起步，按物理通道升序划分扇区。
  首个物理触发锁定本轮方向；纯换向在下一物理轮生效，成像服务、共享内存及重建资源保留。
- 改进 FBP 默认尝试 GPU 融合预处理，保留分段路径；两条路径使用相同精确核和同心圆反投影。
  DAS 与可选 STFT 保留，旧 FilteredDAS 已退出本轮实现。
- 轮次、超时、保存顺序和诊断定义见 PROJECT_STATUS §4。旧 MultiPortReceiver/PacketAssemblyBuffer 不是生产入口。
  Layer 2b T_arr、额外 trigger 的精确来源及正式全孔径校准保持原验证边界。

## 已验收交付

本轮本地包：`artifacts/build-delivery/20261107-215839_4c83ecf7_ring_queue_direction_local.zip`，BuildIdentity 为 `4c83ecf7b0dc0e0be8ca901ca952b7521f79393a`。
标准构建、57/57 CTest、MainWindow 1/1、包内顺逆 × 分段/融合 FBP 四组自检通过。
发布验收文档不会生成新二进制；后续交付仍须针对目标 exact SHA 重新 configure/build。

## 本地工作区

本地旧 main d3054d5 含一份独有文档/开发配置提交，与已验收链分叉，保留不改；
不要 reset/rebase 或隐式接入旧 main 内容。
未跟踪文件、frozen _migration_pack 和旧 _pending_delete 保持原样，最终删除仅由用户执行。
