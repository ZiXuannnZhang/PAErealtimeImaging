# HANDOFF — PAERealtimeImaging 当前接力入口

> 更新时间：2026-11-04（UTC+8）。当前基准为 latest origin/main。

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

## 接手要点

先读 PROJECT_STATUS.md、REPOSITORY_BASELINE.md、BUILD_STANDARD.md，再读本文件与协作指南。
新任务默认从 latest origin/main 新建分支。原低通 tip 3340fbf 和全部历史分支保留追溯。
电机物理轮首触发冻结参数，计数/超时边界后允许应用；startupFilter=0 生效，服务停止不解除闩锁，
窗口编辑暂缓，拒绝后保留重试。参数文件选择后记忆路径，不解除当前物理轮冻结。

Ring 服务先对完整 A-line 做时变低通，再做既有 DBR/限幅/DelayCut，随后 FilteredDAS 和既有对齐/几何/CUDA DAS。
raw/save 与 frontend clone 隔离；成像 cap 后前端信号可持续刷新，publisher/Ring 冻结。
旧 MultiPortReceiver/PacketAssemblyBuffer 不是生产入口。CONFIG 仍需当前事务全卡 60-byte ACK。
RoundIdentity、timeout/save/reset 顺序与统计语义以 PROJECT_STATUS.md §4 为准；CUDA/SHM ABI 未改变。
修改 RingReconCudaConfig 时仍须核对全部 DLL/consumer ABI。RadiusCalibration 仍需真实全孔径校准数据，
不对 raw Bscan 做 circshift。
ring-zero-phase 仅作数值参考，DAS B-tier 已停维护，尚无全孔径 8 通道正式校准完成结论。

## 已验收交付

artifacts/build-delivery/20261029-213219_1fb9478e_lowpass_motor_local_direction/，
BuildIdentity 为 1fb9478e71c95815671c5e1a2c1b3d6a302746d1、tracked-clean；
标准构建、56/56 回归、真实 MainWindow 1/1、合成服务自检通过。
包内旧 local-only/UNVERIFIED 回执保留原样，由本次验收记录补充；不重标旧二进制。
将来正式新交付仍须在目标 exact SHA configure/build，记录 runtime 来源、SHA256 与完整 bin。

## 本地工作区

本地旧 main d3054d5 含一份独有文档/开发配置提交，与已验收链分叉，保留不改；
当前联合分支在发布后对应远端 main。不要 reset/rebase 或隐式接入旧 main 内容。
未跟踪文件、frozen _migration_pack 和旧 _pending_delete 保持原样，最终删除仅由用户执行。
