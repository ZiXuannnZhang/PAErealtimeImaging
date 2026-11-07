# 环形实时成像实机验收记录（2026-11-07，UTC+8）

用户确认此前未验收工作已全部通过实机验收，并授权整理、推送本地工作。最新验收包对应 `4c83ecf7b0dc0e0be8ca901ca952b7521f79393a`。本次确认以用户实际测试范围为准；未收到新增现场日志、逐圈计数或时序数据，不据此推定所有配置的性能或完成独立根因验证。

| 提交 | 验收工作 |
| --- | --- |
| `fe9b87c` / `21d60c0` | 精确核环形 FBP（同心圆折射）；退役旧 FilteredDAS 和可选 DAS 权重控件 |
| `86722a2` / `ae4ef58` | 亚采样校正、批量 GPU STFT、GPU 预处理与 FBP 融合；保留分段与融合两条 FBP 路径 |
| `e3f308d` | 顺逆均从九点钟起步，物理通道升序沿电机方向划分扇区 |
| `feba39f` | 消除瞬时队列锁竞争导致的丢弃；按轮次和逻辑触发序号有界配对；不改变采集直存 |
| `4c83ecf` | 仅联动方向；纯换向按物理轮切换，复用成像进程、共享内存及重建资源；更新状态提示 |

- 实现分支：`codex/ring-realtime-performance-20261104`；本轮实现基点：`6ae7e8940ca7088bb4ea01415da9e303f301a24a`。
- 已验收 `MC_410T_MultiCard` 子树：`8da942be1a382bfff8e6b4520424a58b0a461a7c`。
- 本地交付 ZIP：`artifacts/build-delivery/20261107-215839_4c83ecf7_ring_queue_direction_local.zip`；SHA256：`bbda1190f9111f2ccd34eb7a8412986ae4c2eb6044b0e3b2d665e256c6579624`。包和原始现场材料不随源码推送。
- 最终包以 exact `4c83ecf`、tracked-clean 执行标准 CUDA 12.8.93 和 Qt 6.8/MinGW Debug configure/build，完整核对依赖及 66 个 ZIP 条目。包内 `validation/` 保留编译和测试回执；57/57 CTest 与 1/1 MainWindow 集成测试执行于 `54ba3b4e3857eccdfc20d11aa1901d736e6225e8`，到 `4c83ecf` 只更新模块 README。最终包的程序另通过 2-A、4-A 顺逆 × 分段/融合 FBP 四组自检，每组 3 块。
- 不重标原交付包的 BuildIdentity，也不改写包中当时的 local-only / 待实机说明。八通道性能测试仍暂缓；Layer 2b T_arr、发现早退 B 及额外 trigger 的 FPGA/LabVIEW 来源沿用原验证边界。

此前阶段资料：[精确 FBP](../ring-exact-fbp-20261104/README.md)、[界面与 DAS 清理](../ring-ui-cleanup-20261104/README.md)、[性能优化、融合与起点修正](../ring-realtime-performance-20261104/README.md)。当前状态以 [PROJECT_STATUS.md](../../PROJECT_STATUS.md) 为准。
